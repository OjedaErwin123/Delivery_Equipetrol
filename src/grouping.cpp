#include "grouping.hpp"

#include <algorithm>
#include <future>

Grouping groupByRestaurant(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants)
{
    Grouping g;
    std::vector<std::uint32_t> counts(restaurants, 0);
    for (std::uint32_t r : restaurantOf) {
        ++counts[r];
    }
    g.offsets.assign(restaurants + 1, 0);
    for (std::size_t r = 0; r < restaurants; ++r) {  // scan exclusivo
        g.offsets[r + 1] = g.offsets[r] + counts[r];
    }
    std::vector<std::uint32_t> next(g.offsets.begin(), g.offsets.end() - 1);
    g.order.resize(restaurantOf.size());
    for (std::size_t i = 0; i < restaurantOf.size(); ++i) {
        g.order[next[restaurantOf[i]]++] = static_cast<std::uint32_t>(i);
    }
    return g;
}

Grouping groupByRestaurantParallel(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants,
                                   ThreadPool& pool)
{
    const std::size_t n = restaurantOf.size();
    const std::size_t parts = std::max<std::size_t>(1, std::min(pool.size(), n));
    auto beginOf = [n, parts](std::size_t t) { return n * t / parts; };

    // 1. Cada tramo cuenta lo suyo (contadores privados: sin locks ni atómicos).
    std::vector<std::vector<std::uint32_t>> counts(parts);
    {
        std::vector<std::future<ThreadPool::Outcome<std::vector<std::uint32_t>>>> partial;
        for (std::size_t t = 0; t < parts; ++t) {
            partial.push_back(pool.submit([&restaurantOf, restaurants, begin = beginOf(t), end = beginOf(t + 1)] {
                std::vector<std::uint32_t> own(restaurants, 0);
                for (std::size_t i = begin; i < end; ++i) {
                    ++own[restaurantOf[i]];
                }
                return own;
            }));
        }
        for (std::size_t t = 0; t < parts; ++t) {
            counts[t] = partial[t].get().take();
        }
    }
    // 2. Scan exclusivo sobre (restaurante, tramo), restaurante afuera: dónde empieza cada tramo en cada grupo.
    Grouping g;
    g.offsets.assign(restaurants + 1, 0);
    std::vector<std::vector<std::uint32_t>> start(parts, std::vector<std::uint32_t>(restaurants));
    std::uint32_t running = 0;
    for (std::size_t r = 0; r < restaurants; ++r) {
        g.offsets[r] = running;
        for (std::size_t t = 0; t < parts; ++t) {
            start[t][r] = running;
            running += counts[t][r];
        }
    }
    g.offsets[restaurants] = running;
    // 3. Cada tramo reparte lo suyo, en orden (cada posición la escribe un solo trabajo).
    g.order.resize(n);
    std::vector<std::future<ThreadPool::Outcome<bool>>> done;
    for (std::size_t t = 0; t < parts; ++t) {
        done.push_back(pool.submit([&restaurantOf, &g, next = start[t], begin = beginOf(t), end = beginOf(t + 1)]() mutable {
            for (std::size_t i = begin; i < end; ++i) {
                g.order[next[restaurantOf[i]]++] = static_cast<std::uint32_t>(i);
            }
            return true;
        }));
    }
    for (auto& part : done) {
        part.get().take();
    }
    return g;
}
