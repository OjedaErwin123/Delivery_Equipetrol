#include "report.hpp"

#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "grouping.hpp"
#include "histogram.hpp"
#ifdef DELIVERY_USE_CUDA
#include "gpu/gpu_owner.hpp"
#endif

LogReplay replayLog(std::istream& log)
{
    LogReplay replay;
    std::unordered_map<std::string, std::int64_t> createdAt;
    std::unordered_map<std::string, std::uint32_t> restaurantIndex;
    std::string line;
    for (std::size_t number = 1; std::getline(log, line); ++number) {
        if (line.empty()) {
            continue;
        }
        // Una línea que no es JSON, a la que le falta un campo o que entrega un pedido que no se creó: se informa
        // con su número de línea.
        try {
            const nlohmann::json event = nlohmann::json::parse(line);
            const std::string& kind = event.at("event").get_ref<const std::string&>();
            if (kind == "orderCreated") {
                createdAt[event.at("order").get<std::string>()] = event.at("t").get<std::int64_t>();
                const std::string& restaurant = event.at("restaurant").get_ref<const std::string&>();
                const auto index =
                    restaurantIndex.emplace(restaurant, static_cast<std::uint32_t>(replay.restaurants.size()));
                if (index.second) {
                    replay.restaurants.push_back(restaurant);
                }
                replay.restaurantOf.push_back(index.first->second);
            } else if (kind == "orderDelivered") {
                const std::int64_t took =
                    event.at("t").get<std::int64_t>() - createdAt.at(event.at("order").get<std::string>());
                replay.deliveryMs.push_back(static_cast<std::uint32_t>(took));
                ++replay.delivered;
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("log, línea " + std::to_string(number) + ": " + error.what());
        }
    }
    return replay;
}

void writeReport(const LogReplay& replay, GpuOwner* gpu, std::ostream& out)
{
    nlohmann::json report;

    // H1: los tiempos de entrega por minuto y sus percentiles.
    Histogram counts;
    std::string where = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr) {
        counts = gpu->histogram(replay.deliveryMs, true).get().take().counts;
        where = "GPU (" + gpu->deviceName() + ")";
    }
#else
    (void)gpu;
#endif
    if (counts.empty()) {
        counts = deliveryHistogram(replay.deliveryMs);
    }
    std::uint64_t total = 0;
    for (std::uint64_t c : counts) {
        total += c;
    }
    report["h1_deliveryTimes"] = {
        {"computedOn", where},
        {"binMinutes", 1},
        {"lastBinIsAtLeastMinutes", kDeliveryBins - 1},
        {"counts", counts},
        {"total", total},
        {"deliveredInLog", replay.delivered},
        {"p50Minute", percentileMinute(counts, 50)},
        {"p90Minute", percentileMinute(counts, 90)},
        {"p99Minute", percentileMinute(counts, 99)},
    };

    // SC1: los pedidos agrupados por restaurante (counting sort estable: scan exclusivo de los conteos). Los
    // offsets dicen dónde empieza el grupo de cada restaurante; dentro de cada grupo, los pedidos quedan en el orden
    // de creación. El reporte da, por restaurante, cuántos pedidos tuvo y su primer y último pedido (posición en el
    // orden de creación). Más de GroupingGpu::kMaxRestaurants (64) restaurantes: en la CPU.
    const std::size_t restaurants = replay.restaurants.size();
    Grouping groups;
    where = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr && restaurants <= GroupingGpu::kMaxRestaurants) {
        groups = gpu->group(replay.restaurantOf, restaurants).get().take().grouping;
        where = "GPU (" + gpu->deviceName() + ")";
    }
#endif
    if (groups.offsets.empty()) {
        groups = groupByRestaurant(replay.restaurantOf, restaurants);
    }
    nlohmann::json perRestaurant = nlohmann::json::object();
    for (std::size_t r = 0; r < restaurants; ++r) {
        const std::uint32_t begin = groups.offsets[r], end = groups.offsets[r + 1];
        nlohmann::json entry = {{"orders", end - begin}, {"offset", begin}};
        if (end > begin) {
            entry["firstOrder"] = groups.order[begin];
            entry["lastOrder"] = groups.order[end - 1];
        }
        perRestaurant[replay.restaurants[r]] = entry;
    }
    report["sc1_ordersByRestaurant"] = {
        {"computedOn", where},
        {"created", replay.restaurantOf.size()},
        {"restaurants", perRestaurant},
    };
    out << report.dump(1) << "\n";
}
