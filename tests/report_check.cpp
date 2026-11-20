// Prueba del reporte (--report, EC2): una simulación de verdad con la configuración dada, su log releído
// (replayLog) y el reporte (writeReport, en la CPU). Uso: report_check <config.json>.
//  - H1: un tiempo por pedido entregado; la suma de las cajas = las entregas del resumen = las líneas
//    orderDelivered; el promedio y el máximo recalculados desde el log = los del resumen;
//  - SC1: un restaurante por pedido creado; grupos contiguos, con solo pedidos de su restaurante, en orden de
//    creación; conteos = los recontados;
//  - R2: por restaurante, creados y entregas suman los del resumen; máximo y promedio = los del resumen;
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
    writeReport(replay, nullptr, reportOut);
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

    auto lineOfError =[](const std::string& text) {
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
