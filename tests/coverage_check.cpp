// Prueba de las versiones de CPU de S1 (EC2): el mapa de cobertura por barridos de Jacobi. Sin rodeos (todas las
// celdas con calles, costo 1) y con suficientes barridos, el campo es la distancia de Manhattan al restaurante más
// cercano (por el interior: los bordes no se actualizan); los restaurantes quedan en 0; la de N hilos (1 a 7) da
// exactamente lo de 1 hilo después de cualquier número de barridos, también con rodeos (costo 3). Sale con 0 si
// todo está bien.
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "coverage.hpp"
#include "log_line.hpp"

namespace {

CoverageGrid gridWith(int w, int h, const std::vector<std::pair<int, int>>& shops, bool detours, std::mt19937& random)
{
    CoverageGrid g{w, h, std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h, kStreetCost),
                   std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h, kFar)};
    if (detours) {
        for (auto& c : g.cost) {
            c = random() % 3 == 0 ? kOffStreetCost : kStreetCost;
        }
    }
    for (const auto& s : shops) {
        g.distance[static_cast<std::size_t>(s.second) * w + s.first] = 0;
    }
    return g;
}

}  // namespace

int main()
{
    std::mt19937 random(47);
    bool ok = true;
    {
        const int w = 40, h = 30;
        const std::vector<std::pair<int, int>> shops = {{5, 5}, {30, 20}, {12, 25}};
        const CoverageGrid g = gridWith(w, h, shops, false, random);
        const std::vector<std::uint32_t> d = relaxCoverage(g, w + h);
        bool manhattan = true, zero = true;
        for (int y = 1; y < h - 1; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                int best = 1 << 30;
                for (const auto& s : shops) {
                    best = std::min(best, std::abs(x - s.first) + std::abs(y - s.second));
                }
                manhattan &= d[static_cast<std::size_t>(y) * w + x] == static_cast<std::uint32_t>(best);
            }
        }
        for (const auto& s : shops) {
            zero &= d[static_cast<std::size_t>(s.second) * w + s.first] == 0;
        }
        ok &= check("sin rodeos y con w + h barridos: distancia de Manhattan al restaurante más cercano", manhattan);
        ok &= check("los restaurantes quedan en 0", zero);
    }
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool same = true;
    for (int sweeps : {0, 1, 7, 150}) {
        const CoverageGrid g = gridWith(70, 45, {{3, 3}, {60, 40}, {35, 10}}, true, random);
        const std::vector<std::uint32_t> reference = relaxCoverage(g, sweeps);
        for (auto& pool : pools) {
            same &= relaxCoverageParallel(g, sweeps, *pool) == reference;
        }
    }
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo con rodeos, después de 0, 1, 7 y 150 barridos", same);
    return ok ? 0 : 1;
}
