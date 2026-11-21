// Prueba de igualdad de P2 (EC2): el rodeo de los sentidos únicos en la GPU (real con USE_CUDA=ON, emulada con
// cuda_compat.h), con los tres kernels (ingenuo, corner turning y corner turning con relleno), da EXACTAMENTE lo de la
// CPU (sumas de 64 bits y el peor par con su desempate), con tablas que no son múltiplo de 32, de 0 y 1 nodos y con
// muchos empates. También por el hilo dueño de la GPU. Sale con 0 si todo está bien.
#include <cstdio>
#include <vector>

#include "asymmetry.hpp"
#include "gpu/asymmetry_gpu.hpp"
#include "gpu/cuda_compat.h"
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
    // La emulación lanza un hilo por hilo de cada bloque: con tablas grandes tardaría minutos.
    const std::size_t big = BY_BACKEND(2048, 96);
    AsymmetryGpu gpu((big + 31) / 32 * 32);
    bool same = true;
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{33}, std::size_t{70}, big}) {
        for (std::uint32_t maxExtra : {7u, 3000u}) {  // 7: muchos empates en el peor par
            const DistanceTable table = syntheticTable(n, static_cast<std::uint32_t>(9 + n), maxExtra);
            const Asymmetry cpu = asymmetryNaive(table);
            for (AsymmetryGpu::Kernel k :
                 {AsymmetryGpu::Kernel::naive, AsymmetryGpu::Kernel::tiled, AsymmetryGpu::Kernel::padded}) {
                same &= gpu.compare(table, k) == cpu;
            }
        }
    }
    std::printf("hasta %zu nodos (%s)\n", big, CUDA_BACKEND);
    bool ok = check("ingenuo, corner turning y con relleno = CPU (pares, sumas y el peor par)", same);

    GpuOwner owner(1);
    const DistanceTable table = syntheticTable(50, 3);
    ok &= check("por el hilo dueño de la GPU: lo mismo",
                owner.asymmetry(table, AsymmetryGpu::Kernel::padded).get().take().totals == asymmetryNaive(table));
    return ok ? 0 : 1;
}
