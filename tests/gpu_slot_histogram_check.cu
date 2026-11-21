// Prueba de igualdad de H2 (EC2): el histograma 2D (restaurante, franja) en la GPU (real con USE_CUDA=ON, emulada
// con cuda_compat.h), con el recorrido intercalado y con el contiguo, da EXACTAMENTE lo de la CPU: con 1, 37 y 64
// restaurantes (64 x 144 = 9216 cajas, el máximo en memoria compartida), sin pedidos y con todos en la misma caja.
// También a través del hilo dueño de la GPU. Sale con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/slot_histogram_gpu.hpp"
#include "slot_histogram.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    std::mt19937 random(37);
    const std::size_t big = BY_BACKEND(500000, 3000);
    SlotHistogramGpu gpu(big);
    bool same = true;
    for (std::size_t restaurants : {1, 37, 64}) {
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{257}, big}) {
            std::vector<std::uint32_t> restaurantOf(n), createdMs(n);
            for (std::size_t i = 0; i < n; ++i) {
                restaurantOf[i] = static_cast<std::uint32_t>(random() % restaurants);
                createdMs[i] = random() % (30u * 3600000u);
            }
            const SlotHistogram cpu = ordersPerSlot(restaurantOf, createdMs, restaurants);
            same &= gpu.count(restaurantOf, createdMs, restaurants, true) == cpu &&
                    gpu.count(restaurantOf, createdMs, restaurants, false) == cpu;
        }
    }
    const std::vector<std::uint32_t> one(big, 5u), sameTime(big, 7u * kSlotMs);  // todos en la caja [5][7]
    const SlotHistogram cpu = ordersPerSlot(one, sameTime, 37);
    const bool contention = gpu.count(one, sameTime, 37, true) == cpu && cpu[5 * kSlots + 7] == big;
    std::printf("hasta %zu pedidos (%s)\n", big, CUDA_BACKEND);
    bool ok = check("intercalado y contiguo = CPU, caja por caja (1, 37 y 64 restaurantes)", same);
    ok &= check("todos en la misma caja: el total exacto", contention);

    GpuOwner owner(1);
    std::vector<std::uint32_t> restaurantOf(2000), createdMs(2000);
    for (std::size_t i = 0; i < restaurantOf.size(); ++i) {
        restaurantOf[i] = static_cast<std::uint32_t>(random() % 37);
        createdMs[i] = random() % 3600000u;
    }
    ok &= check("por el hilo dueño de la GPU: los mismos conteos",
                owner.slotHistogram(restaurantOf, createdMs, 37, true).get().take().counts ==
                    ordersPerSlot(restaurantOf, createdMs, 37));
    return ok ? 0 : 1;
}
