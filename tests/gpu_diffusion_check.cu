// Prueba de igualdad de S2 (EC2): la difusión de la demanda en la GPU (real con USE_CUDA=ON, emulada con
// cuda_compat.h), con el kernel básico y con el de coarsening por columnas, da lo de la CPU dentro de la tolerancia
// de diffusion.hpp (1e-5 del mayor valor de la grilla inicial) después de 0, 1, un número impar de pasos y 100; con
// grillas que no son múltiplo del bloque ni de las 8 filas por hilo. También por el hilo dueño de la GPU. Sale con
// 0 si todo está bien.
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

#include "diffusion.hpp"
#include "gpu/cuda_compat.h"
#include "gpu/diffusion_gpu.hpp"
#include "gpu/gpu_owner.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    std::mt19937 random(61);
    const int side = BY_BACKEND(700, 40);
    DiffusionGpu gpu(static_cast<std::size_t>(side) * side);
    float worst = 0;
    bool within = true;
    for (auto size : {std::pair<int, int>{3, 3}, {33, 13}, {side, side}}) {
        Grid in{size.first, size.second, std::vector<float>(static_cast<std::size_t>(size.first) * size.second)};
        for (float& c : in.cells) {
            c = static_cast<float>(random() % 40);
        }
        const float scale = std::max(maxValue(in), 1.0f);
        for (int steps : {0, 1, 7, 100}) {
            const Grid cpu = diffuse(in, steps);
            for (bool coarsened : {false, true}) {
                const float relative = maxDifference(gpu.diffuse(in, steps, coarsened), cpu) / scale;
                worst = std::max(worst, relative);
                within &= relative <= 1e-5f;
            }
        }
    }
    std::printf("mayor diferencia / mayor valor inicial: %.3g (%s)\n", static_cast<double>(worst), CUDA_BACKEND);
    bool ok = check("básico y con coarsening = CPU dentro de 1e-5 del mayor valor inicial", within);

    GpuOwner owner(1);
    Grid in{50, 30, std::vector<float>(1500)};
    for (float& c : in.cells) {
        c = static_cast<float>(random() % 40);
    }
    const Grid cpu = diffuse(in, 20);
    ok &= check("por el hilo dueño de la GPU: dentro de la tolerancia",
                maxDifference(owner.diffusion(in, 20, true).get().take().grid, cpu) <= 1e-5f * maxValue(in));
    return ok ? 0 : 1;
}
