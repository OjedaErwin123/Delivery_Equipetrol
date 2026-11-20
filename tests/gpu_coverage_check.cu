// Prueba de igualdad de S1 (EC2): los barridos de Jacobi del mapa de cobertura en la GPU (real con USE_CUDA=ON,
// emulada con cuda_compat.h), con el kernel básico y con el de bloques con halo en memoria compartida, dan
// EXACTAMENTE lo de la CPU con el mismo número de barridos (0, 1, un número impar y hasta converger), con rodeos y
// con grillas que no son múltiplo del bloque. También por el hilo dueño de la GPU. Sale con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "coverage.hpp"
#include "gpu/coverage_gpu.hpp"
#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

CoverageGrid randomGrid(int w, int h, std::mt19937& random)
{
    CoverageGrid g{w, h, std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h),
                   std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h, kFar)};
    for (auto& c : g.cost) {
        c = random() % 3 == 0 ? kOffStreetCost : kStreetCost;
    }
    for (int k = 0; k < 5; ++k) {
        g.distance[random() % g.distance.size()] = 0;
    }
    return g;
}

}  // namespace

int main()
{
    std::mt19937 random(53);
    const int side = BY_BACKEND(500, 40);
    CoverageGpu gpu(static_cast<std::size_t>(side) * side);
    bool same = true;
    for (auto size : {std::pair<int, int>{3, 3}, {33, 9}, {side, side}}) {
        const CoverageGrid g = randomGrid(size.first, size.second, random);
        for (int sweeps : {0, 1, 7, size.first + size.second}) {
            const std::vector<std::uint32_t> cpu = relaxCoverage(g, sweeps);
            same &= gpu.relax(g, sweeps, false) == cpu && gpu.relax(g, sweeps, true) == cpu;
        }
    }
    std::printf("grillas 3x3, 33x9 y %dx%d (%s)\n", side, side, CUDA_BACKEND);
    bool ok = check("básico y por bloques = CPU, celda por celda, con el mismo número de barridos", same);

    GpuOwner owner(1);
    const CoverageGrid g = randomGrid(37, 29, random);
    ok &= check("por el hilo dueño de la GPU: el mismo campo",
                owner.coverage(g, 66, true).get().take().distance == relaxCoverage(g, 66));
    return ok ? 0 : 1;
}
