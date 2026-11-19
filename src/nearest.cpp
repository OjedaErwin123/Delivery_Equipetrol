#include "nearest.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace {

using Keyed = std::pair<double, std::size_t>;  // (distancia, id): el orden de nearest() del despachador

Keyed keyOf(const Coordinates& at, std::size_t id, Coordinates shop)
{
    return {haversineMeters(at.lat, at.lon, shop.lat, shop.lon), id};
}

std::vector<std::size_t> idsOf(const std::vector<Keyed>& keyed, std::size_t limit)
{
    std::vector<std::size_t> chosen;
    for (std::size_t k = 0; k < limit && k < keyed.size(); ++k) {
        chosen.push_back(keyed[k].second);
    }
    return chosen;
}

}  // namespace

std::vector<std::size_t> nearestOf(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                   Coordinates shop, std::size_t limit)
{
    if (limit == 0 || at.size() <= limit) {
        return ids;
    }
    std::vector<Keyed> keyed;
    keyed.reserve(at.size());
    for (std::size_t p = 0; p < at.size(); ++p) {
        keyed.push_back(keyOf(at[p], ids[p], shop));
    }
    std::sort(keyed.begin(), keyed.end());  // como nearest(): ordena todo y toma las primeras
    return idsOf(keyed, limit);
}

std::vector<std::size_t> nearestOfParallel(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                           Coordinates shop, std::size_t limit, ThreadPool& pool)
{
    if (limit == 0 || at.size() <= limit) {
        return ids;
    }
    const std::size_t parts = std::min(pool.size(), at.size());
    std::vector<std::future<ThreadPool::Outcome<std::vector<Keyed>>>> tops;
    for (std::size_t t = 0; t < parts; ++t) {
        const std::size_t begin = at.size() * t / parts, end = at.size() * (t + 1) / parts;
        // Por referencia: este hilo espera todos los resultados antes de salir, así que `at` e `ids` viven.
        tops.push_back(pool.submit([&at, &ids, shop, limit, begin, end] {
            std::vector<Keyed> keyed;
            keyed.reserve(end - begin);
            for (std::size_t p = begin; p < end; ++p) {
                keyed.push_back(keyOf(at[p], ids[p], shop));
            }
            const std::size_t keep = std::min(limit, keyed.size());
            std::partial_sort(keyed.begin(), keyed.begin() + static_cast<std::ptrdiff_t>(keep), keyed.end());
            keyed.resize(keep);
            return keyed;
        }));
    }
    std::vector<Keyed> merged;
    for (auto& top : tops) {
        const std::vector<Keyed> part = top.get().take();
        merged.insert(merged.end(), part.begin(), part.end());
    }
    std::sort(merged.begin(), merged.end());
    return idsOf(merged, limit);
}

std::vector<std::size_t> nearestWithin(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                       const std::vector<std::size_t>& subset, Coordinates shop, std::size_t limit)
{
    std::vector<Keyed> keyed;
    keyed.reserve(subset.size());
    for (std::size_t p : subset) {
        keyed.push_back(keyOf(at[p], ids[p], shop));
    }
    std::sort(keyed.begin(), keyed.end());
    return idsOf(keyed, limit);
}
