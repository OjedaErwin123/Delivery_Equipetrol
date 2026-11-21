#include "apsp.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <unordered_map>

namespace {

std::size_t paddedSize(std::size_t n)
{
    return (n + DistanceTable::kTile - 1) / DistanceTable::kTile * DistanceTable::kTile;
}

DistanceTable emptyTable(std::size_t n)
{
    DistanceTable table;
    table.n = n;
    table.stride = std::max(paddedSize(n), DistanceTable::kTile);
    table.meters.assign(table.stride * table.stride, DistanceTable::kUnreachable);
    for (std::size_t i = 0; i < table.stride; ++i) {
        table.meters[i * table.stride + i] = 0;
    }
    return table;
}

void addStreet(DistanceTable& table, std::size_t from, std::size_t to, double meters)
{
    const auto rounded = static_cast<std::uint32_t>(std::lround(meters));
    std::uint32_t& cell = table.meters[from * table.stride + to];
    cell = std::min(cell, rounded);
}

// Las filas [begin, end) en el paso k.
void relaxRows(DistanceTable& table, std::size_t k, std::size_t begin, std::size_t end)
{
    const std::size_t s = table.stride;
    const std::uint32_t* rowK = &table.meters[k * s];
    for (std::size_t i = begin; i < end; ++i) {
        if (i == k) {
            continue;  // la fila k no cambia en el paso k (d[k][k] = 0)
        }
        std::uint32_t* row = &table.meters[i * s];
        const std::uint32_t ik = row[k];
        if (ik == DistanceTable::kUnreachable) {
            continue;  // de i no se llega a k: nada que mejorar por k
        }
        for (std::size_t j = 0; j < s; ++j) {
            row[j] = std::min(row[j], ik + rowK[j]);
        }
    }
}

}  // namespace

DistanceTable initialTable(const RoadGraph& graph)
{
    DistanceTable table = emptyTable(graph.nodeCount());
    for (const RoadGraph::DirectedStreet& street : graph.directedStreets()) {
        addStreet(table, street.from, street.to, street.meters);
    }
    return table;
}

DistanceTable initialTable(const RoadGraph& graph, const std::vector<std::size_t>& nodes)
{
    std::unordered_map<std::size_t, std::size_t> position;
    for (std::size_t p = 0; p < nodes.size(); ++p) {
        position.emplace(nodes[p], p);
    }
    DistanceTable table = emptyTable(nodes.size());
    for (const RoadGraph::DirectedStreet& street : graph.directedStreets()) {
        const auto from = position.find(street.from), to = position.find(street.to);
        if (from != position.end() && to != position.end()) {
            addStreet(table, from->second, to->second, street.meters);
        }
    }
    return table;
}

void floydWarshall(DistanceTable& table)
{
    for (std::size_t k = 0; k < table.n; ++k) {
        relaxRows(table, k, 0, table.n);
    }
}

namespace {

constexpr std::size_t kTile = DistanceTable::kTile;

// own[i][j] = min(own[i][j], via[i][k] + to[k][j]) para k = 0..31, sobre bloques de 32 × 32 de la tabla. `own`
// puede ser el mismo bloque que `via` o que `to` (fases 1 y 2): como d[k][k] = 0, la fila y la columna k del
// bloque no cambian en el paso k, así que actualizar en el lugar da lo mismo que en la GPU (enteros: idéntico).
void relaxTile(std::uint32_t* own, const std::uint32_t* via, const std::uint32_t* to, std::size_t stride)
{
    for (std::size_t k = 0; k < kTile; ++k) {
        const std::uint32_t* toRow = to + k * stride;
        for (std::size_t i = 0; i < kTile; ++i) {
            const std::uint32_t ik = via[i * stride + k];
            std::uint32_t* row = own + i * stride;
            for (std::size_t j = 0; j < kTile; ++j) {  // 32 columnas seguidas: el compilador lo vectoriza
                row[j] = std::min(row[j], ik + toRow[j]);
            }
        }
    }
}

// Fase 3: lo mismo, pero `own`, `via` y `to` son tres bloques DISTINTOS y solo `own` cambia. Se copian a arreglos
// locales de 32 × 32 (4 KB cada uno: los tres caben en la caché L1), como la memoria compartida de la GPU. Así el
// compilador sabe que no se pisan y vectoriza el bucle de j; sobre los punteros a la tabla no puede suponerlo (en
// las fases 1 y 2 sí se pisan). Orden i-k-j: la fila i del resultado se queda en registros mientras recorre k.
void relaxTileApart(std::uint32_t* own, const std::uint32_t* via, const std::uint32_t* to, std::size_t stride)
{
    alignas(64) std::uint32_t a[kTile][kTile], b[kTile][kTile], c[kTile][kTile];
    for (std::size_t i = 0; i < kTile; ++i) {
        for (std::size_t j = 0; j < kTile; ++j) {
            a[i][j] = via[i * stride + j];
            b[i][j] = to[i * stride + j];
            c[i][j] = own[i * stride + j];
        }
    }
    for (std::size_t i = 0; i < kTile; ++i) {
        for (std::size_t k = 0; k < kTile; ++k) {
            const std::uint32_t ik = a[i][k];
            for (std::size_t j = 0; j < kTile; ++j) {
                c[i][j] = std::min(c[i][j], ik + b[k][j]);
            }
        }
    }
    for (std::size_t i = 0; i < kTile; ++i) {
        for (std::size_t j = 0; j < kTile; ++j) {
            own[i * stride + j] = c[i][j];
        }
    }
}

// Reparte los trabajos 0..count-1 en pool.size() tramos y espera a todos (fan-out / fan-in).
template <typename Work>
void forEachParallel(ThreadPool& pool, std::size_t count, Work work)
{
    const std::size_t parts = std::min(pool.size(), count);
    std::vector<std::future<ThreadPool::Outcome<bool>>> done;
    for (std::size_t t = 0; t < parts; ++t) {
        const std::size_t begin = count * t / parts, end = count * (t + 1) / parts;
        done.push_back(pool.submit([&work, begin, end] {  // por referencia: se espera a todos antes de salir
            for (std::size_t w = begin; w < end; ++w) {
                work(w);
            }
            return true;
        }));
    }
    for (auto& part : done) {
        part.get().take();
    }
}

}  // namespace

void floydWarshallBlocked(DistanceTable& table, ThreadPool& pool)
{
    const std::size_t s = table.stride, blocks = s / kTile;
    std::uint32_t* d = table.meters.data();
    auto tile = [d, s](std::size_t bi, std::size_t bj) { return d + bi * kTile * s + bj * kTile; };
    for (std::size_t kb = 0; kb < blocks; ++kb) {
        std::uint32_t* pivot = tile(kb, kb);
        relaxTile(pivot, pivot, pivot, s);  // fase 1
        if (blocks == 1) {
            continue;
        }
        // Fase 2: la fila kb y la columna kb (cada bloque lo escribe un solo trabajo; el pivote solo se lee).
        forEachParallel(pool, 2 * (blocks - 1), [&](std::size_t w) {
            const std::size_t t = w % (blocks - 1) < kb ? w % (blocks - 1) : w % (blocks - 1) + 1;
            if (w < blocks - 1) {
                relaxTile(tile(kb, t), pivot, tile(kb, t), s);
            } else {
                relaxTile(tile(t, kb), tile(t, kb), pivot, s);
            }
        });
        // Fase 3: los demás bloques, por filas de bloques (la fila y la columna kb solo se leen).
        forEachParallel(pool, blocks - 1, [&](std::size_t w) {
            const std::size_t bi = w < kb ? w : w + 1;
            for (std::size_t bj = 0; bj < blocks; ++bj) {
                if (bj != kb) {
                    relaxTileApart(tile(bi, bj), tile(bi, kb), tile(kb, bj), s);
                }
            }
        });
    }
}

std::vector<std::vector<bool>> reachabilityFrom(const DistanceTable& table, const std::vector<std::size_t>& restaurants,
                                                const std::vector<std::size_t>& destinations)
{
    std::vector<std::vector<bool>> reachable(restaurants.size(), std::vector<bool>(destinations.size()));
    for (std::size_t r = 0; r < restaurants.size(); ++r) {
        for (std::size_t d = 0; d < destinations.size(); ++d) {
            reachable[r][d] = table.at(restaurants[r], destinations[d]) != DistanceTable::kUnreachable;
        }
    }
    return reachable;
}
