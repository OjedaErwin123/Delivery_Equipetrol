// Prueba de igualdad de C1 (EC2): la convolución gaussiana en la GPU (real con USE_CUDA=ON, emulada con
// cuda_compat.h), con el kernel básico y con el de bloques con halo en memoria compartida, da lo de la CPU dentro de
// la tolerancia de heatmap.hpp: la mayor diferencia, celda por celda, es a lo sumo 1e-5 del mayor valor de la grilla
// (la GPU usa FMA; la suma es la misma y en el mismo orden). Con grillas que no son múltiplo de 16, una de una sola
// celda y una con todo concentrado en una esquina (el halo fuera de la grilla). También por el hilo dueño de la GPU.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/heatmap_gpu.hpp"
#include "heatmap.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    const std::vector<float> filter = gaussianFilter();
    std::mt19937 random(43);
    const int side = BY_BACKEND(1000, 70);
    HeatmapGpu gpu(static_cast<std::size_t>(side) * side);
    float worst = 0;  // la mayor diferencia relativa al máximo
    bool within = true;
    for (auto size : {std::pair<int, int>{1, 1}, {17, 5}, {33, 70}, {side, side}}) {
        Grid in{size.first, size.second, std::vector<float>(static_cast<std::size_t>(size.first) * size.second)};
        for (float& c : in.cells) {
            c = static_cast<float>(random() % 50);
        }
        in.cells[0] = 1000.0f;  // una esquina muy cargada: el halo de fuera de la grilla cuenta como 0
        const Grid cpu = convolve(in, filter);
        for (bool tiled : {false, true}) {
            const float relative = maxDifference(gpu.convolve(in, filter, tiled), cpu) / maxValue(cpu);
            worst = std::max(worst, relative);
            within &= relative <= 1e-5f;
        }
    }
    std::printf("mayor diferencia / mayor valor: %.3g (%s)\n", static_cast<double>(worst), CUDA_BACKEND);
    bool ok = check("básico y por bloques = CPU dentro de 1e-5 del mayor valor de la grilla", within);

    GpuOwner owner(1);
    Grid in{64, 48, std::vector<float>(64 * 48)};
    for (float& c : in.cells) {
        c = static_cast<float>(random() % 50);
    }
    const Grid cpu = convolve(in, filter);
    ok &= check("por el hilo dueño de la GPU: dentro de la tolerancia",
                maxDifference(owner.heatmap(in, filter, true).get().take().grid, cpu) <= 1e-5f * maxValue(cpu));
    return ok ? 0 : 1;
}
