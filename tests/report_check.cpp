// Prueba del reporte (--report, EC2): una simulación de verdad con la configuración dada, su log releído
// (replayLog) y el reporte (writeReport, en la CPU). Uso: report_check <config.json>.
//  - H1: un tiempo por pedido entregado; la suma de las cajas = las entregas del resumen = las líneas
//    orderDelivered; el promedio y el máximo recalculados desde el log = los del resumen;
//  - una línea del log que no es JSON se informa con su número de línea.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <atomic>
#include <iostream>
#include <numeric>
#include <sstream>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "log_line.hpp"
#include "report.hpp"
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

    bool reported = false;
    try {
        std::istringstream bad("{\"t\": 0, \"event\": \"orderCreated\", \"order\": \"o0\"}\nesto no es JSON\n");
        replayLog(bad);
    } catch (const std::runtime_error& error) {
        reported = std::string(error.what()).find("línea 2") != std::string::npos;
        std::cout << "error esperado: \"" << std::string(error.what()).substr(0, 60) << "…\"\n";
    }
    ok &= check("una línea que no es JSON se informa con su número", reported);
    return ok ? 0 : 1;
}
