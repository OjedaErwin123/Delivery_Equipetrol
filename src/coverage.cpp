#include "coverage.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace {

// Un barrido de las filas [y0, y1) de `from` a `to` (solo celdas interiores: los bordes se copiaron al empezar).
void sweepRows(const CoverageGrid& g, const std::vector<std::uint32_t>& from, std::vector<std::uint32_t>& to, int y0,
               int y1)
{
    const int w = g.width;
    for (int y = std::max(y0, 1); y < std::min(y1, g.height - 1); ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const std::size_t c = static_cast<std::size_t>(y) * w + x;
            const std::uint32_t best = std::min(std::min(from[c - 1], from[c + 1]), std::min(from[c - w], from[c + w]));
            to[c] = std::min(from[c], best + g.cost[c]);
        }
    }
}

}  // namespace

std::vector<std::uint32_t> relaxCoverage(const CoverageGrid& grid, int sweeps)
{
    std::vector<std::uint32_t> a = grid.distance, b = grid.distance;  // los bordes, iguales en los dos
    for (int s = 0; s < sweeps; ++s) {
        sweepRows(grid, a, b, 0, grid.height);
        std::swap(a, b);
    }
    return a;
}

std::vector<std::uint32_t> relaxCoverageParallel(const CoverageGrid& grid, int sweeps, ThreadPool& pool)
{
    std::vector<std::uint32_t> a = grid.distance, b = grid.distance;
    const int parts = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(pool.size(), grid.height)));
    for (int s = 0; s < sweeps; ++s) {
        std::vector<std::future<ThreadPool::Outcome<bool>>> done;
        for (int t = 0; t < parts; ++t) {
            const int y0 = grid.height * t / parts, y1 = grid.height * (t + 1) / parts;
            done.push_back(pool.submit([&grid, &a, &b, y0, y1] {  // se espera a todos antes del barrido siguiente
                sweepRows(grid, a, b, y0, y1);
                return true;
            }));
        }
        for (auto& part : done) {
            part.get().take();
        }
        std::swap(a, b);
    }
    return a;
}
