// Prueba del flujo básico: corre la simulación completa (rápida) y verifica el log y
// el resumen como lo haría el revisor. Sale con 0 si todo está bien.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

struct Run {
    Summary summary;
    std::vector<std::string> log;
    std::string out;
};

Run simulate(const SimParams& p)
{
    std::ostringstream logOut;
    std::ostringstream out;
    Run run;
    run.summary = runSimulation(p, logOut, out);
    run.out = out.str();
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        run.log.push_back(line);
    }
    return run;
}

}  // namespace

int main()
{
    SimParams p;
    p.couriers = 3;
    p.orders = 30;
    p.meanIntervalMs = 1000;
    p.legMs = 2000;
    p.timeScale = 10000.0;  // 1 s simulado = 0,1 ms real
    const Run run = simulate(p);

    // Por pedido: los cinco eventos en orden, sin que t retroceda, con la misma moto
    // en assigned/pickup/delivered y repartidos por turnos (o0→c0, o1→c1, ...).
    const std::vector<std::string> lifecycle = {"orderCreated", "orderAssigned", "pickupStarted", "orderPickedUp",
                                                "orderDelivered"};
    std::map<std::string, std::vector<std::string>> events;  // pedido -> eventos en orden
    std::map<std::string, std::int64_t> lastT;
    std::map<std::string, std::string> carrier;
    std::map<std::string, int> deliveredBy;
    std::map<std::string, std::int64_t> createdAt;
    bool timeForward = true;
    bool sameCourier = true;
    std::int64_t total = 0;
    std::int64_t max = 0;
    for (const std::string& line : run.log) {
        const std::string order = field(line, "order");
        const std::string event = field(line, "event");
        const std::int64_t t = timeOf(line);
        timeForward = timeForward && (events[order].empty() || t >= lastT[order]);
        lastT[order] = t;
        events[order].push_back(event);
        if (event == "orderCreated") {
            createdAt[order] = t;
        } else if (event == "orderAssigned") {
            carrier[order] = field(line, "courier");
        } else {
            sameCourier = sameCourier && field(line, "courier") == carrier[order];
        }
        if (event == "orderDelivered") {
            ++deliveredBy[field(line, "courier")];
            total += t - createdAt[order];
            max = std::max(max, t - createdAt[order]);
        }
    }
    bool lifecycleOk = static_cast<int>(events.size()) == p.orders;
    bool roundRobin = true;
    for (int i = 0; i < p.orders; ++i) {
        const std::string order = "o" + std::to_string(i);
        lifecycleOk = lifecycleOk && events[order] == lifecycle;
        roundRobin = roundRobin && carrier[order] == "c" + std::to_string(i % p.couriers);
    }

    const Summary& s = run.summary;
    bool perCourier = static_cast<int>(s.deliveredPerCourier.size()) == p.couriers;
    for (const auto& entry : s.deliveredPerCourier) {
        perCourier = perCourier && deliveredBy[entry.first] == entry.second;
    }

    std::cout << "eventos en el log: " << run.log.size() << "\n" << toJson(s) << "\n";
    bool ok = check("línea de inicio", run.out.rfind("Simulation has started...\n", 0) == 0);
    ok &= check("cada pedido: created, assigned, pickupStarted, pickedUp, delivered", lifecycleOk);
    ok &= check("t nunca retrocede para un pedido", timeForward);
    ok &= check("la moto asignada es la que recoge y entrega", sameCourier);
    ok &= check("reparto por turnos", roundRobin);
    ok &= check("resumen: created = delivered + pending", s.created == p.orders && s.delivered == p.orders);
    ok &= check("resumen: entregas por moto iguales al log", perCourier);
    ok &= check("resumen: tiempos iguales al log",
                s.maxDeliveryMs == max && s.avgDeliveryMs == total / p.orders);

    // Reproducible: con la misma semilla se crean los mismos pedidos para los mismos restaurantes.
    auto created = [](const Run& r) {
        std::vector<std::string> v;
        for (const std::string& line : r.log) {
            if (field(line, "event") == "orderCreated") {
                v.push_back(field(line, "order") + "@" + field(line, "restaurant"));
            }
        }
        return v;
    };
    ok &= check("misma semilla, mismos pedidos", created(simulate(p)) == created(run));
    return ok ? 0 : 1;
}
