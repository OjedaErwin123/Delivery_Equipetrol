// Prueba de igualdad de SC1 (EC2): el counting sort estable de los pedidos por restaurante en la GPU (real con
// USE_CUDA=ON, emulada con cuda_compat.h) da EXACTAMENTE lo de la CPU: el mismo orden (estable) y los mismos
// offsets. Con restaurantes vacíos, con todos los pedidos en un solo restaurante, sin pedidos, con 64 restaurantes
// (el máximo) y con más de un pedazo de 1024 en el scan (más de 1024 pares restaurante-bloque). También a través
// del hilo dueño de la GPU. Sale con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/grouping_gpu.hpp"
#include "grouping.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    std::mt19937 random(13);
    // 64 restaurantes x 160 bloques = 10 240 pares: 10 pedazos del scan. En la emulación (cada bloque son 128
    // std::thread) con 20 000 pedidos ya hay 5 bloques; en la GPU real, 1 000 000 llega a los 160.
    const std::size_t big = BY_BACKEND(1000000, 20000);
    GroupingGpu gpu(big);
    bool same = true;
    for (std::size_t restaurants : {1, 37, 64}) {
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{4097}, big}) {
            std::vector<std::uint32_t> restaurantOf(n);
            for (std::uint32_t& r : restaurantOf) {
                r = static_cast<std::uint32_t>(random() % (restaurants > 3 ? restaurants - 2 : restaurants));
            }
            same &= gpu.group(restaurantOf, restaurants) == groupByRestaurant(restaurantOf, restaurants);
        }
    }
    std::printf("hasta %zu pedidos, %u bloques (%s)\n", big, GroupingGpu::blocksFor(big), CUDA_BACKEND);
    bool ok = check("GPU = CPU: el mismo orden estable y los mismos offsets (1, 37 y 64 restaurantes)", same);

    GpuOwner owner(1);
    std::vector<std::uint32_t> restaurantOf(5000);
    for (std::uint32_t& r : restaurantOf) {
        r = static_cast<std::uint32_t>(random() % 37);
    }
    ok &= check("por el hilo dueño de la GPU: el mismo agrupamiento",
                owner.group(restaurantOf, 37).get().take().grouping == groupByRestaurant(restaurantOf, 37));
    return ok ? 0 : 1;
}
