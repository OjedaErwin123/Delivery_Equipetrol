// Prueba de las versiones de CPU de R1 (EC2): las motos más cercanas al restaurante. nearestOf es el criterio de
// nearest() del despachador; nearestOfParallel (N hilos, un combiner por tramo) debe dar exactamente lo mismo con
// cualquier cantidad de hilos, también con muchos empates (varias motos en el mismo nodo). nearestWithin sobre un
// subconjunto que contiene a las mejores da lo mismo que nearestOf sobre todas. Sale con 0 si todo está bien.
#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "log_line.hpp"
#include "nearest.hpp"
#include "thread_pool.hpp"

int main()
{
    bool parallelSame = true, withinSame = true, tiesSorted = true, smallAsIs = true;
    std::mt19937 random(7);
    // 40 puntos posibles en ~2 km: con cientos de motos, muchas comparten nodo y distancia.
    std::vector<Coordinates> points;
    for (int k = 0; k < 40; ++k) {
        points.push_back({-17.80 + (random() % 2000) * 1e-5, -63.20 + (random() % 2000) * 1e-5});
    }
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    for (std::size_t n : {0, 1, 8, 9, 10, 50, 333, 2000}) {
        for (std::size_t limit : {0, 1, 8, 20}) {
            std::vector<Coordinates> at(n);
            std::vector<std::size_t> ids(n);
            for (std::size_t i = 0; i < n; ++i) {
                at[i] = points[random() % points.size()];
                ids[i] = 3 * i + 1;  // ids crecientes, como fleet.withSpace()
            }
            const Coordinates shop = points[random() % points.size()];
            const std::vector<std::size_t> reference = nearestOf(at, ids, shop, limit);
            for (auto& pool : pools) {
                parallelSame &= nearestOfParallel(at, ids, shop, limit, *pool) == reference;
            }
            if (limit == 0 || n <= limit) {
                smallAsIs &= reference == ids;  // como nearest(): todas, en su orden
                continue;
            }
            // Las elegidas, de menor a mayor (distancia, id).
            for (std::size_t k = 1; k < reference.size(); ++k) {
                const std::size_t a = (reference[k - 1] - 1) / 3, b = (reference[k] - 1) / 3;
                const double da = haversineMeters(at[a].lat, at[a].lon, shop.lat, shop.lon);
                const double db = haversineMeters(at[b].lat, at[b].lon, shop.lat, shop.lon);
                tiesSorted &= da < db || (da == db && reference[k - 1] < reference[k]);
            }
            // Un subconjunto con las mejores y otras al azar (más de `limit`).
            std::vector<std::size_t> subset;
            for (std::size_t id : reference) {
                subset.push_back((id - 1) / 3);
            }
            for (std::size_t extra = 0; extra < 5; ++extra) {
                subset.push_back(random() % n);
            }
            std::sort(subset.begin(), subset.end());
            subset.erase(std::unique(subset.begin(), subset.end()), subset.end());
            if (subset.size() > limit) {
                withinSame &= nearestWithin(at, ids, subset, shop, limit) == reference;
            }
        }
    }
    bool ok = true;
    ok &= check("nearestOfParallel = nearestOf con 1, 2, 3, 4 y 7 hilos (con empates)", parallelSame);
    ok &= check("las elegidas van de menor a mayor (distancia, id): los empates, por menor id", tiesSorted);
    ok &= check("con limit 0 o pocas motos, todas en su orden (como nearest())", smallAsIs);
    ok &= check("nearestWithin sobre un subconjunto con las mejores = nearestOf sobre todas", withinSame);
    return ok ? 0 : 1;
}
