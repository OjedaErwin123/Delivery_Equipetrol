// Prueba de las versiones de CPU de H2 (EC2): el histograma 2D de pedidos por (restaurante, franja de 10 minutos).
// La de N hilos (1 a 7) da exactamente lo de 1 hilo; la suma de una fila es la cantidad de pedidos de ese
// restaurante; los bordes de franja y la última (que junta todo desde 23 h 50 min) caen donde deben. Sale con 0 si
// todo está bien.
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "log_line.hpp"
#include "slot_histogram.hpp"

int main()
{
    std::mt19937 random(31);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool parallel = true, rows = true;
    for (std::size_t restaurants : {1, 37, 64}) {
        for (std::size_t n : {0, 1, 1000, 100000}) {
            std::vector<std::uint32_t> restaurantOf(n), createdMs(n);
            std::vector<std::uint64_t> perRestaurant(restaurants, 0);
            for (std::size_t i = 0; i < n; ++i) {
                restaurantOf[i] = static_cast<std::uint32_t>(random() % restaurants);
                createdMs[i] = random() % (30u * 3600000u);  // hasta 30 h: también la última franja
                ++perRestaurant[restaurantOf[i]];
            }
            const SlotHistogram reference = ordersPerSlot(restaurantOf, createdMs, restaurants);
            for (auto& pool : pools) {
                parallel &= ordersPerSlotParallel(restaurantOf, createdMs, restaurants, *pool) == reference;
            }
            for (std::size_t r = 0; r < restaurants; ++r) {
                rows &= std::accumulate(reference.begin() + static_cast<std::ptrdiff_t>(r * kSlots),
                                        reference.begin() + static_cast<std::ptrdiff_t>((r + 1) * kSlots),
                                        std::uint64_t{0}) == perRestaurant[r];
            }
        }
    }
    bool ok = true;
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo, caja por caja", parallel);
    ok &= check("la fila de cada restaurante suma sus pedidos", rows);
    ok &= check("bordes: 9 min 59 s en la franja 0, 10 min en la 1, 23 h 50 min y 30 h en la última (143)",
                slotOf(599999) == 0 && slotOf(600000) == 1 && slotOf(143u * kSlotMs) == 143 &&
                    slotOf(30u * 3600000u) == 143);
    return ok ? 0 : 1;
}
