#include "phases.hpp"

#include <algorithm>
#include <future>

OrderColumns columnsOf(const std::vector<OrderRecord>& records)
{
    OrderColumns c;
    for (const OrderRecord& r : records) {
        c.createdMs.push_back(r.createdMs);
        c.assignedMs.push_back(r.assignedMs);
        c.pickedUpMs.push_back(r.pickedUpMs);
        c.deliveredMs.push_back(r.deliveredMs);
    }
    return c;
}

PhaseTotals phaseTotals(const std::vector<OrderRecord>& records)
{
    PhaseTotals t;
    for (const OrderRecord& r : records) {
        if (r.deliveredMs != kNotYet) {
            ++t.delivered;
            t.waitMs += r.assignedMs - r.createdMs;
            t.pickupMs += r.pickedUpMs - r.assignedMs;
            t.rideMs += r.deliveredMs - r.pickedUpMs;
        }
    }
    return t;
}

PhaseTotals phaseTotalsParallel(const OrderColumns& c, ThreadPool& pool)
{
    const std::size_t n = c.createdMs.size();
    const std::size_t parts = std::max<std::size_t>(1, std::min(pool.size(), n));
    std::vector<std::future<ThreadPool::Outcome<PhaseTotals>>> partial;
    for (std::size_t p = 0; p < parts; ++p) {
        const std::size_t begin = n * p / parts, end = n * (p + 1) / parts;
        partial.push_back(pool.submit([&c, begin, end] {
            PhaseTotals own;  // sumas privadas del tramo
            for (std::size_t i = begin; i < end; ++i) {
                if (c.deliveredMs[i] != kNotYet) {
                    ++own.delivered;
                    own.waitMs += c.assignedMs[i] - c.createdMs[i];
                    own.pickupMs += c.pickedUpMs[i] - c.assignedMs[i];
                    own.rideMs += c.deliveredMs[i] - c.pickedUpMs[i];
                }
            }
            return own;
        }));
    }
    PhaseTotals t;
    for (auto& part : partial) {
        const PhaseTotals own = part.get().take();
        t.delivered += own.delivered;
        t.waitMs += own.waitMs;
        t.pickupMs += own.pickupMs;
        t.rideMs += own.rideMs;
    }
    return t;
}
