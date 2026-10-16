// Prueba de la generación de pedidos: ráfagas de hasta burstMax en el mismo instante, el tiempo
// medio entre dos pedidos igual a meanIntervalMs, la misma secuencia para una semilla, y el
// límite del libro (maxPending = 0 rechaza todo con queueFull). Sale con 0 si todo está bien.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

struct Run {
    Summary summary;
    std::string json;
    std::vector<std::string> log;
};

Run simulate(const SimParams& p)
{
    std::ostringstream logOut;
    std::ostringstream out;
    Run run;
    run.summary = runSimulation(p, logOut, out);
    run.json = toJson(run.summary);
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        run.log.push_back(line);
    }
    return run;
}

// (t, restaurante, destino no se registra) de cada orderCreated, en el orden en que se crearon.
std::vector<std::pair<std::int64_t, std::string>> createdSequence(const Run& run)
{
    std::vector<std::pair<std::int64_t, std::string>> seq;
    for (const std::string& line : run.log) {
        if (field(line, "event") == "orderCreated") {
            seq.emplace_back(timeOf(line), field(line, "restaurant"));
        }
    }
    return seq;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Ráfagas: 3000 pedidos, hasta 5 por llegada.
    SimParams p;
    p.couriers = 4;
    p.orders = 3000;
    p.meanIntervalMs = 1000;
    p.burstMax = 5;
    p.legMs = 1000;
    p.timeScale = 100000.0;  // 1 s simulado = 0,01 ms real
    const Run run = simulate(p);
    const auto seq = createdSequence(run);
    std::map<std::int64_t, int> perInstant;
    for (const auto& order : seq) {
        ++perInstant[order.first];
    }
    int biggest = 0;
    std::map<int, int> sizes;  // tamaño de ráfaga -> cuántas llegadas
    for (const auto& instant : perInstant) {
        biggest = std::max(biggest, instant.second);
        ++sizes[instant.second];
    }
    const double meanBetween = static_cast<double>(seq.back().first) / static_cast<double>(seq.size());
    std::cout << "pedidos " << seq.size() << " en " << perInstant.size() << " llegadas; tamaños:";
    for (const auto& size : sizes) {
        std::cout << " " << size.first << "x" << size.second;
    }
    std::cout << "; tiempo medio entre pedidos " << meanBetween << " ms\n";
    ok &= check("ráfagas: nunca más de burstMax pedidos en el mismo instante", biggest <= p.burstMax);
    ok &= check("ráfagas: aparecen todos los tamaños de 1 a burstMax", static_cast<int>(sizes.size()) == p.burstMax);
    ok &= check("tiempo medio entre dos pedidos = meanIntervalMs (±10 %)", meanBetween > 900 && meanBetween < 1100);
    ok &= check("sin límite del libro: nada rechazado, todo entregado",
                run.summary.rejected == 0 && run.summary.delivered == run.summary.created &&
                    run.summary.created == p.orders);

    // 2. Reproducible: la misma semilla da la misma secuencia de pedidos (instante y restaurante).
    ok &= check("misma semilla, mismos pedidos", createdSequence(simulate(p)) == seq);
    SimParams other = p;
    other.seed = 7;
    ok &= check("otra semilla, otros pedidos", createdSequence(simulate(other)) != seq);

    // 3. Sin ráfagas (burstMax = 1): un pedido por instante.
    SimParams single = p;
    single.burstMax = 1;
    single.orders = 500;
    const auto singleSeq = createdSequence(simulate(single));
    bool oneEach = true;
    for (std::size_t i = 1; i < singleSeq.size(); ++i) {
        oneEach = oneEach && singleSeq[i].first != singleSeq[i - 1].first;
    }
    ok &= check("burstMax = 1: un pedido por instante", oneEach);

    // 4. maxPending = 0: el libro no acepta nada; cada pedido se crea y se rechaza (queueFull) en
    //    el mismo instante, y no tiene ningún otro evento.
    SimParams none = p;
    none.orders = 50;
    none.burstMax = 3;
    none.maxPending = 0;
    const Run rejected = simulate(none);
    std::map<std::string, std::vector<std::string>> events;  // pedido -> "evento@t"
    for (const std::string& line : rejected.log) {
        const std::string order = field(line, "order");
        if (!order.empty()) {
            const std::string reason = field(line, "reason");
            events[order].push_back(field(line, "event") + (reason.empty() ? "" : ":" + reason) + "@" +
                                    std::to_string(timeOf(line)));
        }
    }
    bool createdThenRejected = events.size() == 50;
    for (const auto& order : events) {
        const std::string t = order.second.front().substr(order.second.front().find('@'));
        createdThenRejected = createdThenRejected && order.second.size() == 2 &&
                              order.second[0] == "orderCreated" + t && order.second[1] == "orderRejected:queueFull" + t;
    }
    ok &= check("maxPending = 0: cada pedido orderCreated y orderRejected queueFull, mismo t", createdThenRejected);
    ok &= check("maxPending = 0: resumen con 50 rechazados, 0 entregados, 0 pendientes",
                rejected.summary.created == 50 && rejected.summary.rejected == 50 && rejected.summary.delivered == 0 &&
                    rejected.json.find("\"rejected\": 50, \"pending\": 0") != std::string::npos);
    return ok ? 0 : 1;
}
