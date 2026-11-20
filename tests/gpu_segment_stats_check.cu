// Prueba de igualdad de R2 (EC2): la reducción segmentada por restaurante en la GPU (real con USE_CUDA=ON, emulada
// con cuda_compat.h) da EXACTAMENTE lo de la CPU (entregas, suma en 64 bits y máximo), con restaurantes vacíos,
// grupos más chicos y más grandes que un bloque, y pedidos sin entregar. También a través del hilo dueño de la GPU.
// Sale con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/segment_stats_gpu.hpp"
#include "grouping.hpp"
#include "segment_stats.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

std::vector<std::uint32_t> times(std::size_t n, std::mt19937& random)
{
    std::vector<std::uint32_t> ms(n);
    for (std::uint32_t& value : ms) {
        value = random() % 4 == 0 ? kNotDelivered : 60000 + random() % 7200000;
    }
    return ms;
}

}  // namespace

int main()
{
    std::mt19937 random(23);
    const std::size_t big = BY_BACKEND(1000000, 5000);
    SegmentStatsGpu gpu(big, 64);
    bool same = true;
    for (std::size_t restaurants : {1, 37, 64}) {
        for (std::size_t n : {std::size_t{0}, std::size_t{3}, std::size_t{300}, big}) {
            std::vector<std::uint32_t> restaurantOf(n);
            for (std::uint32_t& r : restaurantOf) {
                r = static_cast<std::uint32_t>(random() % (restaurants > 2 ? restaurants - 2 : restaurants));
            }
            const Grouping groups = groupByRestaurant(restaurantOf, restaurants);
            const std::vector<std::uint32_t> ms = times(n, random);
            same &= gpu.stats(groups, ms) == statsPerRestaurant(groups, ms);
        }
    }
    std::printf("hasta %zu pedidos (%s)\n", big, CUDA_BACKEND);
    bool ok = check("GPU = CPU por restaurante (entregas, suma y máximo), con 1, 37 y 64 restaurantes", same);

    GpuOwner owner(1);
    std::vector<std::uint32_t> restaurantOf(3000);
    for (std::uint32_t& r : restaurantOf) {
        r = static_cast<std::uint32_t>(random() % 37);
    }
    const Grouping groups = groupByRestaurant(restaurantOf, 37);
    const std::vector<std::uint32_t> ms = times(restaurantOf.size(), random);
    ok &= check("por el hilo dueño de la GPU: las mismas estadísticas",
                owner.segmentStats(groups, ms).get().take().stats == statsPerRestaurant(groups, ms));
    return ok ? 0 : 1;
}
