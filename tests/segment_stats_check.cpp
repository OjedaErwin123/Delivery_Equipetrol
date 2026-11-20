// Prueba de las versiones de CPU de R2 (EC2): por restaurante, entregas, suma y máximo de los tiempos de entrega
// sobre los grupos de SC1. La de 1 hilo coincide con una cuenta directa pedido por pedido (sin agrupar) y la de N
// hilos (1 a 7) con la de 1 hilo; los pedidos sin entregar no cuentan; un restaurante sin pedidos da todo 0. Sale
// con 0 si todo está bien.
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "grouping.hpp"
#include "log_line.hpp"
#include "segment_stats.hpp"

int main()
{
    std::mt19937 random(21);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool direct = true, parallel = true, empty = true;
    for (std::size_t restaurants : {1, 37}) {
        for (std::size_t n : {0, 1, 1000, 100000}) {
            std::vector<std::uint32_t> restaurantOf(n), ms(n);
            for (std::size_t i = 0; i < n; ++i) {
                restaurantOf[i] = static_cast<std::uint32_t>(random() % (restaurants > 1 ? restaurants - 1 : 1));
                ms[i] = random() % 4 == 0 ? kNotDelivered : 60000 + random() % 7200000;  // 1 de cada 4 sin entregar
            }
            std::vector<RestaurantStats> expected(restaurants);
            for (std::size_t i = 0; i < n; ++i) {
                if (ms[i] != kNotDelivered) {
                    RestaurantStats& s = expected[restaurantOf[i]];
                    ++s.delivered;
                    s.totalMs += ms[i];
                    s.maxMs = std::max(s.maxMs, ms[i]);
                }
            }
            const Grouping groups = groupByRestaurant(restaurantOf, restaurants);
            const std::vector<RestaurantStats> single = statsPerRestaurant(groups, ms);
            direct &= single == expected;
            for (auto& pool : pools) {
                parallel &= statsPerRestaurantParallel(groups, ms, *pool) == single;
            }
            if (restaurants > 1) {
                empty &= single.back() == RestaurantStats{};  // el último restaurante no tiene pedidos
            }
        }
    }
    bool ok = true;
    ok &= check("1 hilo = la cuenta directa pedido por pedido (sin entregar no cuenta)", direct);
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo", parallel);
    ok &= check("un restaurante sin pedidos: 0 entregas, suma 0, máximo 0", empty);
    return ok ? 0 : 1;
}
