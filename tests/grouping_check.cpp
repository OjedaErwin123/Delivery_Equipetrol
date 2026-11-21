// Prueba de las versiones de CPU de SC1 (EC2): los pedidos agrupados por restaurante. La de 1 hilo y la de N hilos
// (1 a 7) dan exactamente lo que da std::stable_sort por restaurante: el mismo orden (estable: dentro de un grupo,
// en el orden de creación) y los offsets = el scan exclusivo de los conteos. También con restaurantes sin pedidos,
// con un solo restaurante y sin pedidos. Sale con 0 si todo está bien.
#include <algorithm>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "grouping.hpp"
#include "log_line.hpp"

namespace {

Grouping byStableSort(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants)
{
    Grouping g;
    g.order.resize(restaurantOf.size());
    std::iota(g.order.begin(), g.order.end(), 0u);
    std::stable_sort(g.order.begin(), g.order.end(),
                     [&restaurantOf](std::uint32_t a, std::uint32_t b) { return restaurantOf[a] < restaurantOf[b]; });
    g.offsets.assign(restaurants + 1, 0);
    for (std::uint32_t r : restaurantOf) {
        ++g.offsets[r + 1];
    }
    std::partial_sum(g.offsets.begin(), g.offsets.end(), g.offsets.begin());
    return g;
}

}  // namespace

int main()
{
    std::mt19937 random(9);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool single = true, parallel = true;
    for (std::size_t restaurants : {1, 7, 37, 64}) {
        for (std::size_t n : {0, 1, 5, 1000, 100000}) {
            std::vector<std::uint32_t> restaurantOf(n);
            for (std::uint32_t& r : restaurantOf) {
                r = static_cast<std::uint32_t>(random() % (restaurants > 3 ? restaurants - 2 : restaurants));  // los últimos, vacíos
            }
            const Grouping reference = byStableSort(restaurantOf, restaurants);
            single &= groupByRestaurant(restaurantOf, restaurants) == reference;
            for (auto& pool : pools) {
                parallel &= groupByRestaurantParallel(restaurantOf, restaurants, *pool) == reference;
            }
        }
    }
    bool ok = true;
    ok &= check("1 hilo = std::stable_sort por restaurante (orden y offsets)", single);
    ok &= check("N hilos (1, 2, 3, 4 y 7) = std::stable_sort por restaurante", parallel);
    return ok ? 0 : 1;
}
