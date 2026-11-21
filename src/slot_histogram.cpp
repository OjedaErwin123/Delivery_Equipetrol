#include "slot_histogram.hpp"

#include <algorithm>
#include <future>

SlotHistogram ordersPerSlot(const std::vector<std::uint32_t>& restaurantOf, const std::vector<std::uint32_t>& createdMs,
                            std::size_t restaurants)
{
    SlotHistogram counts(restaurants * kSlots, 0);
    for (std::size_t i = 0; i < restaurantOf.size(); ++i) {
        ++counts[restaurantOf[i] * kSlots + slotOf(createdMs[i])];
    }
    return counts;
}

SlotHistogram ordersPerSlotParallel(const std::vector<std::uint32_t>& restaurantOf,
                                    const std::vector<std::uint32_t>& createdMs, std::size_t restaurants,
                                    ThreadPool& pool)
{
    const std::size_t n = restaurantOf.size();
    const std::size_t parts = std::max<std::size_t>(1, std::min(pool.size(), n));
    std::vector<std::future<ThreadPool::Outcome<SlotHistogram>>> partial;
    for (std::size_t t = 0; t < parts; ++t) {
        const std::size_t begin = n * t / parts, end = n * (t + 1) / parts;
        partial.push_back(pool.submit([&restaurantOf, &createdMs, restaurants, begin, end] {
            SlotHistogram own(restaurants * kSlots, 0);  // privado de este tramo
            for (std::size_t i = begin; i < end; ++i) {
                ++own[restaurantOf[i] * kSlots + slotOf(createdMs[i])];
            }
            return own;
        }));
    }
    SlotHistogram counts(restaurants * kSlots, 0);
    for (auto& part : partial) {
        const SlotHistogram own = part.get().take();
        for (std::size_t b = 0; b < counts.size(); ++b) {
            counts[b] += own[b];
        }
    }
    return counts;
}
