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
//  - S1: el campo de cobertura convergió; toda celda con calles llega a un restaurante; percentiles en orden;
//  - S2: parte de la demanda de C1; queda a lo sumo kKeep^pasos del total; el pico baja;
//  - P1: entregados = los del resumen; las tres fases suman los tiempos de entrega;
//  - una línea del log que no es JSON, o a la que le falta un campo, se informa con su número de línea.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "diffusion.hpp"
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

    // S1: el campo convergió; todas las celdas con calles (fuera del borde) llegan a algún restaurante (en
    // equipetrol.json todo es alcanzable); p50 <= p90 <= máximo; la celda peor cubierta está dentro del mapa.
    const nlohmann::json& s1 = report.at("s1_coverage");
    std::cout << "S1: " << s1.at("streetCells") << " celdas con calles, costo p50 " << s1.at("p50Cost") << " / p90 "
              << s1.at("p90Cost") << " / máx " << s1.at("maxCost") << " (celda de " << s1.at("cellMeters")
              << " m) · calculado en " << s1.at("computedOn") << "\n";
    ok &= check("S1: el campo convergió (un barrido más no lo cambia)", s1.at("converged").get<bool>());
    ok &= check("S1: todas las celdas con calles llegan a un restaurante",
                s1.at("streetCells").get<int>() > 0 && s1.at("unreachedStreetCells").get<int>() == 0);
    const nlohmann::json& worst = s1.at("worstCovered");
    ok &= check("S1: p50 <= p90 <= máximo y la peor celda dentro del mapa",
                s1.at("p50Cost").get<int>() <= s1.at("p90Cost").get<int>() &&
                    s1.at("p90Cost").get<int>() <= s1.at("maxCost").get<int>() &&
                    worst.at("lat").get<double>() >= b.at("minLat").get<double>() - 1e-9 &&
                    worst.at("lat").get<double>() <= b.at("maxLat").get<double>() + 1e-9);

    // S2: parte de la misma demanda que C1; cada paso apaga kKeep y por los bordes se pierde, así que al final queda
    // a lo sumo kKeep^pasos del total; el pico baja (la demanda se reparte); celdas calientes de mayor a menor.
    const nlohmann::json& s2 = report.at("s2_demandDiffusion");
    const double startTotal = s2.at("totalStart").get<double>(), endTotal = s2.at("totalEnd").get<double>();
    const double decay = std::pow(static_cast<double>(kKeep), s2.at("steps").get<int>());
    const nlohmann::json& hot2 = s2.at("hotspots");
    bool ordered2 = !hot2.empty();
    for (std::size_t k = 1; k < hot2.size(); ++k) {
        ordered2 &= hot2[k - 1].at("value").get<double>() >= hot2[k].at("value").get<double>();
    }
    std::cout << "S2: " << s2.at("steps") << " pasos, total " << startTotal << " -> " << endTotal << ", pico "
              << s2.at("peakStart") << " -> " << s2.at("peakEnd") << " · calculado en " << s2.at("computedOn")
              << "\n";
    ok &= check("S2: parte de la demanda de C1 (un pedido por pedido creado)", startTotal == placed);
    ok &= check("S2: al final queda a lo sumo kKeep^pasos del total, y algo queda",
                endTotal <= startTotal * decay * (1 + 1e-4) && endTotal > 0);
    ok &= check("S2: el pico baja y las celdas calientes van de mayor a menor",
                s2.at("peakEnd").get<double>() < s2.at("peakStart").get<double>() && ordered2);

    // P1: los entregados = los del resumen; esperar + recoger + llevar = el tiempo de entrega de cada pedido, así que
    // las tres sumas juntas = la suma de los tiempos de entrega (recalculada desde el log, sin la GPU).
    const nlohmann::json& p1 = report.at("p1_phases");
    const std::uint64_t phaseSum = p1.at("totalMs").at("wait").get<std::uint64_t>() +
                                   p1.at("totalMs").at("pickup").get<std::uint64_t>() +
                                   p1.at("totalMs").at("ride").get<std::uint64_t>();
    std::cout << "P1: promedio esperar " << p1.at("avgMs").at("wait") << " ms, recoger " << p1.at("avgMs").at("pickup")
              << " ms, llevar " << p1.at("avgMs").at("ride") << " ms · calculado en " << p1.at("computedOn") << "\n";
    ok &= check("P1: entregados = los del resumen", p1.at("delivered").get<int>() == s.delivered);
    ok &= check("P1: esperar + recoger + llevar = la suma de los tiempos de entrega", phaseSum == sum);

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
