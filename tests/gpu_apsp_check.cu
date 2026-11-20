// Prueba de igualdad de T1 (EC2): Floyd–Warshall por bloques en la GPU (real con USE_CUDA=ON, emulada en la CPU
// con cuda_compat.h) da EXACTAMENTE la tabla de floydWarshall en la CPU (enteros: bit a bit). Uso: gpu_apsp_check
// <config.json>. Con la red entera de la configuración y con subredes de varios tamaños (uno que no es múltiplo
// de 32, uno de un solo bloque), y a través del hilo dueño de la GPU. Sale con 0 si todo está bien.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "apsp.hpp"
#include "config.hpp"
#include "gpu/apsp_gpu.hpp"
#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "uso: gpu_apsp_check <config.json>\n");
        return 1;
    }
    const Config config = loadConfig(argv[1]);
    const RoadGraph graph(config);
    // En la GPU real, también la red entera. En la emulación, cada bloque de 32 × 32 son 1024 std::thread: hasta
    // 77 nodos (3 bloques por lado) alcanza para probar las tres fases y el relleno, en segundos.
    const std::size_t largest = BY_BACKEND(graph.nodeCount(), std::min<std::size_t>(77, graph.nodeCount()));
    auto subnet = [&graph](std::size_t n) {
        std::vector<std::size_t> nodes;
        for (std::size_t i = 0; i < n && i < graph.nodeCount(); ++i) {
            nodes.push_back(i);
        }
        return initialTable(graph, nodes);
    };
    bool same = true;
    {
        ApspGpu gpu(DistanceTable::kTile * ((largest + DistanceTable::kTile - 1) / DistanceTable::kTile));
        for (std::size_t n : {std::size_t{20}, std::size_t{32}, std::size_t{45}, largest}) {
            DistanceTable cpu = subnet(n);
            DistanceTable onGpu = cpu;
            floydWarshall(cpu);
            gpu.solve(onGpu);
            same &= onGpu.meters == cpu.meters;
        }
    }
    std::printf("subredes de 20, 32, 45 y %zu nodos (%s)\n", largest, CUDA_BACKEND);
    bool ok = check("GPU por bloques = CPU, bit a bit, en todos los tamaños", same);

    GpuOwner owner(1);
    DistanceTable cpu = subnet(largest);
    const GpuOwner::Apsp solved = owner.apsp(cpu).get().take();
    floydWarshall(cpu);
    ok &= check("por el hilo dueño de la GPU: la misma tabla", solved.table.meters == cpu.meters);
    return ok ? 0 : 1;
}
