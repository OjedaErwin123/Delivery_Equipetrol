// Prueba del reporte (--report, EC2): una simulación de verdad con la configuración dada, su log releído
// (replayLog) y el reporte (writeReport, en la CPU). Uso: report_check <config.json>.
//  - H1: un tiempo por pedido entregado; la suma de las cajas = las entregas del resumen = las líneas
//    orderDelivered; el promedio y el máximo recalculados desde el log = los del resumen;
//  - SC1: un restaurante por pedido creado; grupos contiguos, con solo pedidos de su restaurante, en orden de
//    creación; conteos = los recontados;
//  - R2: por restaurante, creados y entregas suman los del resumen; máximo y promedio = los del resumen;
//  - H2: las franjas de cada restaurante suman sus pedidos; cada pedido en la franja de su t; el pico es el máximo;
//  - C1: los pedidos en la grilla = creados; lo suavizado conserva el total salvo bordes; celdas calientes en orden y
//    dentro del mapa; sin mapa no aparece;
//  - una línea del log que no es JSON, o a la que le falta un campo, se informa con su número de línea.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <atomic>
#include <iostream>
#include <numeric>
#include <sstream>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "grouping.hpp"
#include "log_line.hpp"
#include "report.hpp"
#include "segment_stats.hpp"
#include "slot_histogram.hpp"
#include "simulation.hpp"

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "uso: report_check <config.json>\n";
        return 1;
    }
    SimParams p = paramsFromConfig(loadConfig(argv[1]));
    p.orders = 120;
    p.durationS = 0;
    p.timeScale = 20000.0;  // 1 s real = 5 h 33 min simulados: la corrida tarda unos segundos
    p.acceptTimeoutMs = 1000000000;
    const std::atomic<bool> never(false);
    std::ostringstream logOut, out;
    const Summary s = runSimulation(p, logOut, out, never);

    std::istringstream log(logOut.str());
    const LogReplay replay = replayLog(log);
    std::ostringstream reportOut;
    writeReport(replay, p.world.get(), nullptr, reportOut);
    const nlohmann::json report = nlohmann::json::parse(reportOut.str());
    const nlohmann::json& h1 = report.at("h1_deliveryTimes");
    std::cout << "entregas " << s.delivered << " · p50 " << h1.at("p50Minute") << " min · p90 " << h1.at("p90Minute")
              << " min · p99 " << h1.at("p99Minute") << " min · calculado en " << h1.at("computedOn") << "\n";

    bool ok = true;
    ok &= check("un tiempo por pedido entregado (= delivered del resumen = líneas orderDelivered)",
                s.delivered > 0 && replay.deliveryMs.size() == static_cast<std::size_t>(s.delivered) &&
                    replay.delivered == static_cast<std::uint64_t>(s.delivered));
    ok &= check("la suma de las cajas = las entregas", h1.at("total").get<int>() == s.delivered &&
                                                           h1.at("deliveredInLog").get<int>() == s.delivered);
    const std::uint64_t sum = std::accumulate(replay.deliveryMs.begin(), replay.deliveryMs.end(), std::uint64_t{0});
    const std::uint32_t most = *std::max_element(replay.deliveryMs.begin(), replay.deliveryMs.end());
    ok &= check("promedio y máximo desde el log = avgDeliveryMs y maxDeliveryMs del resumen",
                static_cast<std::int64_t>(sum / replay.deliveryMs.size()) == s.avgDeliveryMs &&
                    static_cast<std::int64_t>(most) == s.maxDeliveryMs);
    ok &= check("p50 <= p90 <= p99", h1.at("p50Minute").get<int>() <= h1.at("p90Minute").get<int>() &&
                                         h1.at("p90Minute").get<int>() <= h1.at("p99Minute").get<int>());

    // SC1: un pedido por línea orderCreated; los grupos cubren todos los pedidos sin huecos; cada grupo tiene solo
    // pedidos de su restaurante, en orden de creación (estable); los conteos = los recontados desde el log.
    const nlohmann::json& sc1 = report.at("sc1_ordersByRestaurant");
    std::cout << "SC1: " << sc1.at("created") << " pedidos en " << replay.restaurants.size()
              << " restaurantes · calculado en " << sc1.at("computedOn") << "\n";
    ok &= check("un restaurante por pedido creado (= created del resumen)",
                replay.restaurantOf.size() == static_cast<std::size_t>(s.created) &&
                    sc1.at("created").get<int>() == s.created);
    const Grouping groups = groupByRestaurant(replay.restaurantOf, replay.restaurants.size());
    bool contiguous = true, own = true, stable = true, counts = true;
    std::uint32_t expectedOffset = 0;
    for (std::size_t r = 0; r < replay.restaurants.size(); ++r) {
        const nlohmann::json& entry = sc1.at("restaurants").at(replay.restaurants[r]);
        const auto mine = static_cast<std::uint32_t>(
            std::count(replay.restaurantOf.begin(), replay.restaurantOf.end(), static_cast<std::uint32_t>(r)));
        counts &= entry.at("orders").get<std::uint32_t>() == mine;
        contiguous &= entry.at("offset").get<std::uint32_t>() == expectedOffset;
        expectedOffset += mine;
        for (std::uint32_t k = groups.offsets[r]; k < groups.offsets[r + 1]; ++k) {
            own &= replay.restaurantOf[groups.order[k]] == r;
            stable &= k == groups.offsets[r] || groups.order[k - 1] < groups.order[k];
        }
    }
    ok &= check("los conteos por restaurante = los recontados desde el log", counts);
    ok &= check("los grupos van uno detrás del otro, sin huecos (scan exclusivo)",
                contiguous && expectedOffset == replay.restaurantOf.size());
    ok &= check("cada grupo tiene solo pedidos de su restaurante, en orden de creación", own && stable);

    // R2: por restaurante, creados = su grupo; la suma de las entregas = delivered del resumen; el promedio
    // ponderado y el máximo entre restaurantes = los del resumen (recalculados desde el log).
    const nlohmann::json& r2 = report.at("r2_perRestaurant");
    std::int64_t r2Delivered = 0, r2Created = 0, r2Max = 0, r2Total = 0;
    for (const auto& entry : r2.at("restaurants").items()) {
        const nlohmann::json& v = entry.value();
        r2Created += v.at("created").get<std::int64_t>();
        r2Delivered += v.at("delivered").get<std::int64_t>();
        r2Max = std::max(r2Max, v.at("maxDeliveryMs").get<std::int64_t>());
    }
    for (std::uint32_t ms : replay.orderDeliveryMs) {
        r2Total += ms == kNotDelivered ? 0 : ms;
    }
    std::cout << "R2: " << r2Delivered << " entregas en " << r2.at("restaurants").size()
              << " restaurantes · calculado en " << r2.at("computedOn") << "\n";
    ok &= check("R2: creados y entregas por restaurante suman los del resumen",
                r2Created == s.created && r2Delivered == s.delivered);
    ok &= check("R2: el máximo entre restaurantes y el promedio de todo = los del resumen",
                r2Max == s.maxDeliveryMs && r2Total / r2Delivered == s.avgDeliveryMs);

    // H2: la suma de las franjas de cada restaurante = sus pedidos creados (SC1); cada pedido cae en la franja de su
    // t (recontado desde el log); el pico es el máximo de su fila.
    const nlohmann::json& h2 = report.at("h2_ordersPerSlot");
    bool slotSums = true, slotCounts = true, peaks = true;
    for (std::size_t r = 0; r < replay.restaurants.size(); ++r) {
        const nlohmann::json& entry = h2.at("restaurants").at(replay.restaurants[r]);
        const auto counts = entry.at("counts").get<std::vector<std::uint64_t>>();
        std::vector<std::uint64_t> recount(counts.size(), 0);
        for (std::size_t i = 0; i < replay.restaurantOf.size(); ++i) {
            if (replay.restaurantOf[i] == r) {
                const std::size_t s = replay.orderCreatedMs[i] / kSlotMs;
                slotCounts &= s < recount.size();
                if (s < recount.size()) {
                    ++recount[s];
                }
            }
        }
        slotCounts &= recount == counts;
        slotSums &= std::accumulate(counts.begin(), counts.end(), std::uint64_t{0}) ==
                    sc1.at("restaurants").at(replay.restaurants[r]).at("orders").get<std::uint64_t>();
        peaks &= entry.at("peakOrders").get<std::uint64_t>() == *std::max_element(counts.begin(), counts.end()) &&
                 counts[entry.at("peakSlot").get<std::size_t>()] == entry.at("peakOrders").get<std::uint64_t>();
    }
    std::cout << "H2: " << h2.at("slotsShown") << " franjas de " << h2.at("slotMinutes") << " min · calculado en "
              << h2.at("computedOn") << "\n";
    ok &= check("H2: las franjas de cada restaurante suman sus pedidos creados (= SC1)", slotSums);
    ok &= check("H2: cada pedido en la franja de su t, recontado desde el log", slotCounts);
    ok &= check("H2: el pico es el máximo de la fila", peaks);

    // C1: cada pedido creado se coloca en una celda (la de su restaurante); el filtro suma 1, así que lo suavizado
    // conserva el total salvo lo que se derrama por los bordes; las celdas calientes van de mayor a menor, dentro de
    // los límites; sin mapa, la sección no aparece.
    const nlohmann::json& c1 = report.at("c1_demandHeatmap");
    const nlohmann::json& hot = c1.at("hotspots");
    const nlohmann::json& b = c1.at("bounds");
    bool ordered = !hot.empty(), inside = true;
    for (std::size_t k = 0; k < hot.size(); ++k) {
        ordered &= k == 0 || hot[k - 1].at("value").get<double>() >= hot[k].at("value").get<double>();
        inside &= hot[k].at("lat").get<double>() >= b.at("minLat").get<double>() - 1e-9 &&
                  hot[k].at("lat").get<double>() <= b.at("maxLat").get<double>() + 1e-9 &&
                  hot[k].at("lon").get<double>() >= b.at("minLon").get<double>() - 1e-9 &&
                  hot[k].at("lon").get<double>() <= b.at("maxLon").get<double>() + 1e-9;
    }
    const double placed = c1.at("ordersPlaced").get<double>(), smoothed = c1.at("smoothedTotal").get<double>();
    std::cout << "C1: " << placed << " pedidos en la grilla de " << c1.at("width") << " x " << c1.at("height")
              << ", suavizado " << smoothed << ", celda más caliente " << hot[0].at("value") << " · calculado en "
              << c1.at("computedOn") << "\n";
    ok &= check("C1: un pedido por pedido creado en la grilla", placed == s.created);
    ok &= check("C1: lo suavizado conserva el total (salvo bordes) y no lo supera",
                smoothed <= placed * (1 + 1e-5) && smoothed >= 0.5 * placed);
    ok &= check("C1: celdas calientes de mayor a menor y dentro del mapa", ordered && inside);
    std::ostringstream noMap;
    writeReport(replay, nullptr, nullptr, noMap);
    ok &= check("C1: sin mapa, el reporte no la incluye",
                nlohmann::json::parse(noMap.str()).count("c1_demandHeatmap") == 0);

    auto lineOfError = [](const std::string& text) {
        try {
            std::istringstream bad(text);
            replayLog(bad);
        } catch (const std::runtime_error& error) {
            std::cout << "error esperado: \"" << std::string(error.what()).substr(0, 60) << "…\"\n";
            return std::string(error.what());
        }
        return std::string();
    };
    const std::string created = "{\"t\": 0, \"event\": \"orderCreated\", \"order\": \"o0\", \"restaurant\": \"r0\"}\n";
    ok &= check("una línea que no es JSON se informa con su número",
                lineOfError(created + "esto no es JSON\n").find("línea 2") != std::string::npos);
    ok &= check("un orderCreated sin restaurante se informa con su número",
                lineOfError(created + "{\"t\": 5, \"event\": \"orderCreated\", \"order\": \"o1\"}\n").find("línea 2") !=
                    std::string::npos);
    return ok ? 0 : 1;
}
