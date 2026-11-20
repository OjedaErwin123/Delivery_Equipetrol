#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "grouping.hpp"
#include "thread_pool.hpp"

// EC2, R2 (reducción segmentada): por restaurante, cuántos de sus pedidos se entregaron, la suma de sus tiempos de
// entrega (para el promedio) y el máximo. Los segmentos son los grupos de SC1 (grouping.hpp): el restaurante r son
// los pedidos order[offsets[r] .. offsets[r+1]). deliveryMs[i] es el tiempo de entrega del pedido i, o
// kNotDelivered. Enteros (la suma en 64 bits): la CPU y la GPU dan exactamente lo mismo.
constexpr std::uint32_t kNotDelivered = 0xffffffffu;

struct RestaurantStats {
    std::uint32_t delivered = 0;
    std::uint64_t totalMs = 0;
    std::uint32_t maxMs = 0;
    bool operator==(const RestaurantStats& o) const
    {
        return delivered == o.delivered && totalMs == o.totalMs && maxMs == o.maxMs;
    }
};

// CPU, 1 hilo.
std::vector<RestaurantStats> statsPerRestaurant(const Grouping& groups, const std::vector<std::uint32_t>& deliveryMs);

// CPU, N hilos: los restaurantes (segmentos independientes) se reparten entre los trabajos del pool; cada uno escribe
// solo los suyos.
std::vector<RestaurantStats> statsPerRestaurantParallel(const Grouping& groups,
                                                        const std::vector<std::uint32_t>& deliveryMs, ThreadPool& pool);
