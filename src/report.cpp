#include "report.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "coverage.hpp"
#include "grouping.hpp"
#include "heatmap.hpp"
#include "histogram.hpp"
#include "segment_stats.hpp"
#include "slot_histogram.hpp"
#ifdef DELIVERY_USE_CUDA
#include "gpu/gpu_owner.hpp"
#endif

LogReplay replayLog(std::istream& log)
{
    LogReplay replay;
    std::unordered_map<std::string, std::int64_t> createdAt;
    std::unordered_map<std::string, std::size_t> createdIndex;  // posición en el orden de creación
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
                const std::string order = event.at("order").get<std::string>();
                createdAt[order] = event.at("t").get<std::int64_t>();
                createdIndex[order] = replay.orderDeliveryMs.size();
                replay.orderDeliveryMs.push_back(kNotDelivered);
                replay.orderCreatedMs.push_back(static_cast<std::uint32_t>(createdAt[order]));
                const std::string& restaurant = event.at("restaurant").get_ref<const std::string&>();
                const auto index =
                    restaurantIndex.emplace(restaurant, static_cast<std::uint32_t>(replay.restaurants.size()));
                if (index.second) {
                    replay.restaurants.push_back(restaurant);
                }
                replay.restaurantOf.push_back(index.first->second);
            } else if (kind == "orderDelivered") {
                const std::string order = event.at("order").get<std::string>();
                const std::int64_t took = event.at("t").get<std::int64_t>() - createdAt.at(order);
                replay.deliveryMs.push_back(static_cast<std::uint32_t>(took));
                replay.orderDeliveryMs[createdIndex.at(order)] = static_cast<std::uint32_t>(took);
                ++replay.delivered;
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("log, línea " + std::to_string(number) + ": " + error.what());
        }
    }
    return replay;
}

namespace {

constexpr int kHeatmapSide = 128;   // C1: celdas por lado de la grilla del mapa de calor
constexpr int kHotspots = 5;        // C1: celdas más calientes que se informan
constexpr int kCoverageSide = 128;  // S1: celdas por lado de la grilla de cobertura

// Una grilla de side x side sobre los límites de la red vial (norte arriba): la celda de una coordenada y el centro
// de una celda. La comparten C1 y S1.
struct CityGrid {
    int side = 0;
    double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    double latSpan = 0, lonSpan = 0;

    CityGrid(const RoadGraph& graph, int cells) : side(cells)
    {
        for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
            const Coordinates& at = graph.coordinates(i);
            minLat = std::min(minLat, at.lat), maxLat = std::max(maxLat, at.lat);
            minLon = std::min(minLon, at.lon), maxLon = std::max(maxLon, at.lon);
        }
        latSpan = std::max(maxLat - minLat, 1e-9), lonSpan = std::max(maxLon - minLon, 1e-9);
    }
    int xOf(double lon) const { return static_cast<int>((lon - minLon) / lonSpan * (side - 1) + 0.5); }
    int yOf(double lat) const { return static_cast<int>((maxLat - lat) / latSpan * (side - 1) + 0.5); }
    std::size_t cellOf(const Coordinates& at) const
    {
        return static_cast<std::size_t>(yOf(at.lat)) * side + static_cast<std::size_t>(xOf(at.lon));
    }
    double latOf(int y) const { return maxLat - latSpan * y / (side - 1); }
    double lonOf(int x) const { return minLon + lonSpan * x / (side - 1); }
    nlohmann::json bounds() const
    {
        return {{"minLat", minLat}, {"maxLat", maxLat}, {"minLon", minLon}, {"maxLon", maxLon}};
    }
};

// C1: el mapa de calor de la demanda. Una grilla de kHeatmapSide x kHeatmapSide sobre los límites de la red; cada
// pedido creado suma 1 en la celda de su restaurante; después, la convolución gaussiana (en la GPU, por bloques; o en
// la CPU). Informa las celdas más calientes con su centro en coordenadas.
nlohmann::json heatmapReport(const LogReplay& replay, const World& world, GpuOwner* gpu)
{
    const RoadGraph& graph = world.graph;
    const CityGrid city(graph, kHeatmapSide);
    std::vector<std::size_t> cellOfRestaurant;
    for (const std::string& r : replay.restaurants) {
        cellOfRestaurant.push_back(city.cellOf(graph.coordinates(world.restaurantNode.at(r))));
    }
    Grid in;
    in.width = in.height = kHeatmapSide;
    in.cells.assign(static_cast<std::size_t>(kHeatmapSide) * kHeatmapSide, 0.0f);
    for (std::uint32_t r : replay.restaurantOf) {
        in.cells[cellOfRestaurant[r]] += 1.0f;
    }

    const std::vector<float> filter = gaussianFilter();
    Grid out;
    std::string where = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr) {
        out = gpu->heatmap(in, filter, true).get().take().grid;
        where = "GPU (" + gpu->deviceName() + ")";
    }
#else
    (void)gpu;
#endif
    if (out.cells.empty()) {
        out = convolve(in, filter);
    }
    std::vector<std::size_t> order(out.cells.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    const std::size_t shown = std::min<std::size_t>(kHotspots, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(shown), order.end(),
                      [&out](std::size_t a, std::size_t b) {
                          return out.cells[a] != out.cells[b] ? out.cells[a] > out.cells[b] : a < b;
                      });
    nlohmann::json hotspots = nlohmann::json::array();
    for (std::size_t k = 0; k < shown; ++k) {
        const int x = static_cast<int>(order[k] % kHeatmapSide), y = static_cast<int>(order[k] / kHeatmapSide);
        hotspots.push_back(
            {{"x", x}, {"y", y}, {"lat", city.latOf(y)}, {"lon", city.lonOf(x)}, {"value", out.cells[order[k]]}});
    }
    double placed = 0, smoothed = 0;
    for (float v : in.cells) {
        placed += v;
    }
    for (float v : out.cells) {
        smoothed += v;
    }
    return {
        {"computedOn", where},
        {"width", kHeatmapSide},
        {"height", kHeatmapSide},
        {"bounds", city.bounds()},
        {"filter", {{"taps", kTaps}, {"sigmaCells", kSigmaCells}}},
        {"ordersPlaced", placed},
        {"smoothedTotal", smoothed},
        {"hotspots", hotspots},
    };
}

// S1: el mapa de cobertura. Una grilla de kCoverageSide x kCoverageSide sobre la ciudad: las celdas por donde pasa
// una calle cuestan kStreetCost y las demás kOffStreetCost; las de los restaurantes (todos los del mapa) valen 0.
// 4 · lado barridos de Jacobi (en la GPU, por bloques; o en la CPU) y se comprueba que el campo ya no cambie con un
// barrido más. Informa, sobre las celdas con calles, cuánto cuesta llegar al restaurante más cercano (en celdas
// recorridas por calle: con el lado de la celda en metros, una distancia aproximada) y la celda peor cubierta.
nlohmann::json coverageReport(const World& world, GpuOwner* gpu)
{
    const RoadGraph& graph = world.graph;
    const CityGrid city(graph, kCoverageSide);
    const std::size_t n = static_cast<std::size_t>(kCoverageSide) * kCoverageSide;
    CoverageGrid grid{kCoverageSide, kCoverageSide, std::vector<std::uint32_t>(n, kOffStreetCost),
                      std::vector<std::uint32_t>(n, kFar)};
    for (const RoadGraph::DirectedStreet& street : graph.directedStreets()) {
        // La calle, rasterizada: puntos a lo largo del segmento, uno por celda que cruza.
        const Coordinates& a = graph.coordinates(street.from);
        const Coordinates& b = graph.coordinates(street.to);
        const int steps = 1 + std::max(std::abs(city.xOf(a.lon) - city.xOf(b.lon)),
                                       std::abs(city.yOf(a.lat) - city.yOf(b.lat)));
        for (int k = 0; k <= steps; ++k) {
            const double f = static_cast<double>(k) / steps;
            grid.cost[city.cellOf({a.lat + (b.lat - a.lat) * f, a.lon + (b.lon - a.lon) * f})] = kStreetCost;
        }
    }
    for (const auto& restaurant : world.restaurantNode) {
        grid.distance[city.cellOf(graph.coordinates(restaurant.second))] = 0;
    }
    const int sweeps = 4 * kCoverageSide;
    std::vector<std::uint32_t> distance;
    std::string where = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr) {
        distance = gpu->coverage(grid, sweeps, true).get().take().distance;
        where = "GPU (" + gpu->deviceName() + ")";
    }
#else
    (void)gpu;
#endif
    if (distance.empty()) {
        distance = relaxCoverage(grid, sweeps);
    }
    CoverageGrid settled = grid;
    settled.distance = distance;
    const bool converged = relaxCoverage(settled, 1) == distance;

    // El lado de la celda en metros (de oeste a este, a la latitud del medio).
    const double midLat = (city.minLat + city.maxLat) / 2;
    const double cellMeters = haversineMeters(midLat, city.minLon, midLat, city.maxLon) / (kCoverageSide - 1);
    std::vector<std::uint32_t> streetCosts;
    std::size_t unreached = 0, worst = 0;
    for (std::size_t c = 0; c < n; ++c) {
        const std::size_t x = c % kCoverageSide, y = c / kCoverageSide;
        const bool border = x == 0 || y == 0 || x + 1 == kCoverageSide || y + 1 == kCoverageSide;
        if (grid.cost[c] != kStreetCost || border) {  // el borde no se actualiza (coverage.hpp): no cuenta
            continue;
        }
        if (distance[c] >= kFar) {
            ++unreached;
            continue;
        }
        streetCosts.push_back(distance[c]);
        if (streetCosts.size() == 1 || distance[c] > distance[worst]) {
            worst = c;
        }
    }
    std::sort(streetCosts.begin(), streetCosts.end());
    auto percentile = [&streetCosts](int p) {
        return streetCosts.empty() ? 0u : streetCosts[(streetCosts.size() - 1) * static_cast<std::size_t>(p) / 100];
    };
    double sum = 0;
    for (std::uint32_t d : streetCosts) {
        sum += d;
    }
    const int wx = static_cast<int>(worst % kCoverageSide), wy = static_cast<int>(worst / kCoverageSide);
    return {
        {"computedOn", where},
        {"width", kCoverageSide},
        {"height", kCoverageSide},
        {"bounds", city.bounds()},
        {"cellMeters", cellMeters},
        {"costs", {{"street", kStreetCost}, {"offStreet", kOffStreetCost}}},
        {"sweeps", sweeps},
        {"converged", converged},
        {"restaurants", world.restaurantNode.size()},
        {"streetCells", streetCosts.size() + unreached},
        {"unreachedStreetCells", unreached},
        {"meanCost", streetCosts.empty() ? 0.0 : sum / streetCosts.size()},
        {"p50Cost", percentile(50)},
        {"p90Cost", percentile(90)},
        {"maxCost", streetCosts.empty() ? 0u : streetCosts.back()},
        {"worstCovered", {{"x", wx}, {"y", wy}, {"lat", city.latOf(wy)}, {"lon", city.lonOf(wx)}}},
    };
}

}  // namespace

void writeReport(const LogReplay& replay, const World* world, GpuOwner* gpu, std::ostream& out)
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
    // R2: por restaurante, entregas, promedio y máximo del tiempo de entrega (reducción segmentada sobre los grupos).
    std::vector<RestaurantStats> stats;
    std::string statsWhere = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr) {
        stats = gpu->segmentStats(groups, replay.orderDeliveryMs).get().take().stats;
        statsWhere = "GPU (" + gpu->deviceName() + ")";
    }
#endif
    if (stats.empty()) {
        stats = statsPerRestaurant(groups, replay.orderDeliveryMs);
    }
    nlohmann::json statsPerName = nlohmann::json::object();
    for (std::size_t r = 0; r < restaurants; ++r) {
        const RestaurantStats& s = stats[r];
        statsPerName[replay.restaurants[r]] = {
            {"created", groups.offsets[r + 1] - groups.offsets[r]},
            {"delivered", s.delivered},
            {"avgDeliveryMs", s.delivered == 0 ? 0 : s.totalMs / s.delivered},
            {"maxDeliveryMs", s.maxMs},
        };
    }
    report["r2_perRestaurant"] = {{"computedOn", statsWhere}, {"restaurants", statsPerName}};

    // H2: los pedidos creados por (restaurante, franja de 10 minutos simulados): las horas pico de cada restaurante.
    // Por restaurante, los conteos de las franjas hasta la última con pedidos, y su franja pico (la primera si hay
    // empate). Más de kMaxSlotRestaurants (64) restaurantes: en la CPU.
    SlotHistogram slots;
    std::string slotsWhere = "CPU";
#ifdef DELIVERY_USE_CUDA
    if (gpu != nullptr && restaurants <= kMaxSlotRestaurants) {
        slots = gpu->slotHistogram(replay.restaurantOf, replay.orderCreatedMs, restaurants, true).get().take().counts;
        slotsWhere = "GPU (" + gpu->deviceName() + ")";
    }
#endif
    if (slots.empty()) {
        slots = ordersPerSlot(replay.restaurantOf, replay.orderCreatedMs, restaurants);
    }
    std::size_t lastSlot = 0;  // la última franja con algún pedido, de cualquier restaurante
    for (std::size_t r = 0; r < restaurants; ++r) {
        for (std::size_t s = 0; s < kSlots; ++s) {
            if (slots[r * kSlots + s] > 0) {
                lastSlot = std::max(lastSlot, s);
            }
        }
    }
    nlohmann::json slotsPerName = nlohmann::json::object();
    for (std::size_t r = 0; r < restaurants; ++r) {
        const auto row = slots.begin() + static_cast<std::ptrdiff_t>(r * kSlots);
        const auto peak = std::max_element(row, row + static_cast<std::ptrdiff_t>(kSlots));
        slotsPerName[replay.restaurants[r]] = {
            {"counts", std::vector<std::uint64_t>(row, row + static_cast<std::ptrdiff_t>(lastSlot + 1))},
            {"peakSlot", peak - row},
            {"peakOrders", *peak},
        };
    }
    report["h2_ordersPerSlot"] = {
        {"computedOn", slotsWhere},
        {"slotMinutes", kSlotMs / 60000},
        {"slotsShown", lastSlot + 1},
        {"restaurants", slotsPerName},
    };

    if (world != nullptr) {
        report["c1_demandHeatmap"] = heatmapReport(replay, *world, gpu);
        report["s1_coverage"] = coverageReport(*world, gpu);
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
