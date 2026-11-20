#include "report.hpp"

#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "histogram.hpp"
#ifdef DELIVERY_USE_CUDA
#include "gpu/gpu_owner.hpp"
#endif

LogReplay replayLog(std::istream& log)
{
    LogReplay replay;
    std::unordered_map<std::string, std::int64_t> createdAt;
    std::string line;
    for (std::size_t number = 1; std::getline(log, line); ++number) {
        if (line.empty()) {
            continue;
        }
        nlohmann::json event;
        try {
            event = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception& error) {
            throw std::runtime_error("log, línea " + std::to_string(number) + ": " + error.what());
        }
        const std::string& kind = event.at("event").get_ref<const std::string&>();
        if (kind == "orderCreated") {
            createdAt[event.at("order").get<std::string>()] = event.at("t").get<std::int64_t>();
        } else if (kind == "orderDelivered") {
            const std::int64_t took = event.at("t").get<std::int64_t>() - createdAt.at(event.at("order").get<std::string>());
            replay.deliveryMs.push_back(static_cast<std::uint32_t>(took));
            ++replay.delivered;
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
    out << report.dump(1) << "\n";
}
