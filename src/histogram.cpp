#include "histogram.hpp"

#include <algorithm>
#include <future>

Histogram deliveryHistogram(const std::vector<std::uint32_t>& ms)
{
    Histogram counts(kDeliveryBins, 0);
    for (std::uint32_t value : ms) {
        ++counts[binOf(value)];
    }
    return counts;
}

Histogram deliveryHistogramParallel(const std::vector<std::uint32_t>& ms, ThreadPool& pool)
{
    const std::size_t parts = std::max<std::size_t>(1, std::min(pool.size(), ms.size()));
    std::vector<std::future<ThreadPool::Outcome<Histogram>>> partial;
    for (std::size_t t = 0; t < parts; ++t) {
        const std::size_t begin = ms.size() * t / parts, end = ms.size() * (t + 1) / parts;
        partial.push_back(pool.submit([&ms, begin, end] {  // por referencia: se espera a todos antes de salir
            Histogram own(kDeliveryBins, 0);                  // el histograma privado de este tramo
            for (std::size_t i = begin; i < end; ++i) {
                ++own[binOf(ms[i])];
            }
            return own;
        }));
    }
    Histogram counts(kDeliveryBins, 0);
    for (auto& part : partial) {
        const Histogram own = part.get().take();
        for (std::size_t b = 0; b < kDeliveryBins; ++b) {
            counts[b] += own[b];
        }
    }
    return counts;
}

int percentileMinute(const Histogram& histogram, double p)
{
    std::uint64_t total = 0;
    for (std::uint64_t count : histogram) {
        total += count;
    }
    if (total == 0) {
        return -1;
    }
    // El menor b con acumulado * 100 >= p * total (en enteros para p con hasta dos decimales: sin redondeo).
    const auto needed = static_cast<std::uint64_t>(p * 100.0 + 0.5);  // p en centésimas de por ciento
    std::uint64_t running = 0;
    for (std::size_t b = 0; b < histogram.size(); ++b) {
        running += histogram[b];
        if (running * 10000 >= needed * total) {
            return static_cast<int>(b);
        }
    }
    return static_cast<int>(histogram.size()) - 1;
}
