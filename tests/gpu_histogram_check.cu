// Prueba de igualdad de H1 (EC2): los dos kernels del histograma (atómicos globales y privatizado en memoria
// compartida) dan EXACTAMENTE los conteos de la CPU, en una GPU real (USE_CUDA=ON) o emulada (cuda_compat.h): con
// tiempos al azar, con todos iguales (la contención máxima: todos los hilos en la misma caja), con cero entregas y
// con más entregas que hilos de la grid (cada hilo cuenta varias). También a través del hilo dueño de la GPU. Sale
// con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/histogram_gpu.hpp"
#include "histogram.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    std::mt19937 random(3);
    // En la emulación cada bloque son 256 std::thread: tamaños chicos. En la GPU real, también uno que necesita
    // más de una vuelta del bucle de paso de grid (160 bloques x 256 hilos = 40 960).
    const std::size_t big = BY_BACKEND(200000, 3000);
    HistogramGpu gpu(big);
    bool same = true, contention = true;
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{255}, std::size_t{257}, big}) {
        std::vector<std::uint32_t> ms(n);
        for (std::uint32_t& value : ms) {
            value = random() % (300u * kBinMs);
        }
        const Histogram cpu = deliveryHistogram(ms);
        same &= gpu.privatized(ms) == cpu && gpu.globalAtomics(ms) == cpu;
    }
    const std::vector<std::uint32_t> equal(big, 42u * kBinMs + 17);  // todas en la caja 42
    const Histogram cpu = deliveryHistogram(equal);
    contention &= gpu.privatized(equal) == cpu && gpu.globalAtomics(equal) == cpu && cpu[42] == big;
    std::printf("tamaños 0, 1, 255, 257 y %zu (%s)\n", big, CUDA_BACKEND);
    bool ok = check("privatizado y atómicos globales = CPU, caja por caja", same);
    ok &= check("todas en la misma caja (contención máxima): los dos dan el total exacto", contention);

    GpuOwner owner(1);
    std::vector<std::uint32_t> ms(1000);
    for (std::uint32_t& value : ms) {
        value = random() % (300u * kBinMs);
    }
    ok &= check("por el hilo dueño de la GPU: los mismos conteos",
                owner.histogram(ms, true).get().take().counts == deliveryHistogram(ms));
    return ok ? 0 : 1;
}
