#include "asymmetry.hpp"

#include <algorithm>
#include <future>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::size_t kTile = DistanceTable::kTile;

// Un par i < j: ida `go` = d[i][j], vuelta `back` = d[j][i].
void comparePair(std::uint32_t go, std::uint32_t back, std::size_t i, std::size_t j, Asymmetry& acc)
{
    const bool there = go < DistanceTable::kUnreachable, home = back < DistanceTable::kUnreachable;
    if (there != home) {
        ++acc.oneWay;
        return;
    }
    if (!there) {
        return;  // sin camino en ningún sentido
    }
    ++acc.pairs;
    if (go != back) {
        const std::uint32_t diff = go > back ? go - back : back - go;
        ++acc.asymmetric;
        acc.sumDiff += diff;
        const std::uint64_t key = (static_cast<std::uint64_t>(diff) << 32) |
                                  (0xffffffffu - static_cast<std::uint32_t>(i << 16 | j));
        acc.worst = std::max(acc.worst, key);
    }
}

void checkSize(const DistanceTable& table)
{
    if (table.n > kMaxAsymmetryNodes) {
        throw std::invalid_argument("asimetría: más de 65535 nodos");
    }
}

// Los pares del bloque (I, J), J >= I, leyendo el espejo (J, I) por columnas desde la caché.
void compareBlock(const DistanceTable& t, std::size_t I, std::size_t J, Asymmetry& acc)
{
    const std::size_t iEnd = std::min(t.n, (I + 1) * kTile), jEnd = std::min(t.n, (J + 1) * kTile);
    for (std::size_t i = I * kTile; i < iEnd; ++i) {
        for (std::size_t j = std::max(J * kTile, i + 1); j < jEnd; ++j) {
            comparePair(t.meters[i * t.stride + j], t.meters[j * t.stride + i], i, j, acc);
        }
    }
}

Asymmetry blockRow(const DistanceTable& t, std::size_t I)
{
    Asymmetry own;
    const std::size_t tiles = (t.n + kTile - 1) / kTile;
    for (std::size_t J = I; J < tiles; ++J) {
        compareBlock(t, I, J, own);
    }
    return own;
}

}  // namespace

void Asymmetry::add(const Asymmetry& o)
{
    pairs += o.pairs;
    oneWay += o.oneWay;
    asymmetric += o.asymmetric;
    sumDiff += o.sumDiff;
    worst = std::max(worst, o.worst);
}

Asymmetry asymmetryNaive(const DistanceTable& t)
{
    checkSize(t);
    Asymmetry acc;
    for (std::size_t i = 0; i < t.n; ++i) {
        for (std::size_t j = i + 1; j < t.n; ++j) {
            comparePair(t.meters[i * t.stride + j], t.meters[j * t.stride + i], i, j, acc);
        }
    }
    return acc;
}

Asymmetry asymmetryBlocked(const DistanceTable& t)
{
    checkSize(t);
    Asymmetry acc;
    const std::size_t tiles = (t.n + kTile - 1) / kTile;
    for (std::size_t I = 0; I < tiles; ++I) {
        acc.add(blockRow(t, I));
    }
    return acc;
}

Asymmetry asymmetryParallel(const DistanceTable& t, ThreadPool& pool)
{
    checkSize(t);
    const std::size_t tiles = (t.n + kTile - 1) / kTile;
    std::vector<std::future<ThreadPool::Outcome<Asymmetry>>> rows;
    for (std::size_t I = 0; I < tiles; ++I) {
        rows.push_back(pool.submit([&t, I] { return blockRow(t, I); }));
    }
    Asymmetry acc;
    for (auto& row : rows) {
        acc.add(row.get().take());
    }
    return acc;
}

DistanceTable syntheticTable(std::size_t n, std::uint32_t seed, std::uint32_t maxExtra)
{
    DistanceTable t;
    t.n = n;
    t.stride = (n + kTile - 1) / kTile * kTile;
    t.meters.assign(t.stride * t.stride, DistanceTable::kUnreachable);
    std::mt19937 random(seed);
    for (std::size_t i = 0; i < n; ++i) {
        t.meters[i * t.stride + i] = 0;
        for (std::size_t j = i + 1; j < n; ++j) {
            const std::uint32_t base = 100 + random() % 20000;
            std::uint32_t go = base, back = base;
            const std::uint32_t kind = random() % 1000;
            if (kind < 5) {
                go = back = DistanceTable::kUnreachable;  // sin camino
            } else if (kind < 15) {
                (random() % 2 == 0 ? go : back) = DistanceTable::kUnreachable;  // un solo sentido
            } else if (kind < 365) {
                (random() % 2 == 0 ? go : back) += 1 + random() % maxExtra;  // un sentido da la vuelta
            }
            t.meters[i * t.stride + j] = go;
            t.meters[j * t.stride + i] = back;
        }
    }
    return t;
}
