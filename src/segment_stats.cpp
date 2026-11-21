#include "segment_stats.hpp"

#include <algorithm>
#include <future>

namespace {

RestaurantStats reduceSegment(const Grouping& groups, const std::vector<std::uint32_t>& deliveryMs, std::size_t r)
{
    RestaurantStats s;
    for (std::uint32_t k = groups.offsets[r]; k < groups.offsets[r + 1]; ++k) {
        const std::uint32_t ms = deliveryMs[groups.order[k]];
        if (ms != kNotDelivered) {
            ++s.delivered;
            s.totalMs += ms;
            s.maxMs = std::max(s.maxMs, ms);
        }
    }
    return s;
}

}  // namespace

std::vector<RestaurantStats> statsPerRestaurant(const Grouping& groups, const std::vector<std::uint32_t>& deliveryMs)
{
    std::vector<RestaurantStats> stats(groups.offsets.size() - 1);
    for (std::size_t r = 0; r < stats.size(); ++r) {
        stats[r] = reduceSegment(groups, deliveryMs, r);
    }
    return stats;
}

std::vector<RestaurantStats> statsPerRestaurantParallel(const Grouping& groups,
                                                        const std::vector<std::uint32_t>& deliveryMs, ThreadPool& pool)
{
    std::vector<RestaurantStats> stats(groups.offsets.size() - 1);
    const std::size_t parts = std::max<std::size_t>(1, std::min(pool.size(), stats.size()));
    std::vector<std::future<ThreadPool::Outcome<bool>>> done;
    for (std::size_t t = 0; t < parts; ++t) {
        // Restaurantes intercalados (t, t + parts, …): reparte mejor si los grupos tienen tamaños muy distintos.
        done.push_back(pool.submit([&groups, &deliveryMs, &stats, t, parts] {
            for (std::size_t r = t; r < stats.size(); r += parts) {
                stats[r] = reduceSegment(groups, deliveryMs, r);
            }
            return true;
        }));
    }
    for (auto& part : done) {
        part.get().take();
    }
    return stats;
}
