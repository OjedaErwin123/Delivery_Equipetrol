// Prueba de la avería con traspaso: en simulaciones con averías cada pedido está siempre en una
// sola mochila (cada orderReassigned sale de quien lo tiene y lo que sigue lo
// hace el receptor), la moto averiada ya no hace nada con lo traspasado, y el resumen cuenta los
// traspasos igual que el log. Sale con 0 si todo está bien.
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido; r0 en b y r1 en d (1 m por ms simulado).
Config smallTown(double breakdownProbability, std::int64_t durationS, int couriers)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 2, 0, 0}};
    c.couriers = couriers;
    c.bagCapacity = 3;
    c.speedKmh = 3600;
    c.startNode = "a";
    c.meanIntervalMs = 5000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 11;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = 100000000;
    c.breakdownProbability = breakdownProbability;
    c.durationS = durationS;
    c.timeScale = 1000;
    return c;
}

struct Run {
    Summary summary;
    std::vector<std::string> lines;
};

Run simulate(const Config& c, int orders)
{
    SimParams p = paramsFromConfig(c);
    p.orders = orders;
    std::ostringstream logOut, out;
    Run run;
    run.summary = runSimulation(p, logOut, out);
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        run.lines.push_back(line);
    }
    return run;
}

int count(const Run& run, const std::string& event)
{
    int n = 0;
    for (const std::string& line : run.lines) {
        n += field(line, "event") == event;
    }
    return n;
}

// Un solo dueño por pedido: asignado a X; cada traspaso sale del dueño hacia otra moto; lo que
// recoge o entrega lo hace el dueño de ese momento.
bool oneOwner(const Run& run)
{
    std::map<std::string, std::string> holder;
    for (const std::string& line : run.lines) {
        const std::string event = field(line, "event"), order = field(line, "order");
        if (event == "orderAssigned") {
            holder[order] = field(line, "courier");
        } else if (event == "orderReassigned") {
            if (holder[order] != field(line, "from") || field(line, "to") == field(line, "from")) {
                return false;
            }
            holder[order] = field(line, "to");
        } else if (event == "pickupStarted" || event == "orderPickedUp" || event == "orderDelivered") {
            if (holder[order] != field(line, "courier")) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Probabilidad 0,3 con tres motos: hay traspasos y cada pedido tiene un solo dueño.
    const Run sometimes = simulate(smallTown(0.3, 7200, 3), 40);
    std::cout << "p = 0,3: " << toJson(sometimes.summary) << "\n";
    ok &= check("p = 0,3: hay traspasos, contados igual que en el log",
                sometimes.summary.reassigned > 0 && sometimes.summary.reassigned == count(sometimes, "orderReassigned") &&
                    sometimes.summary.breakdowns == count(sometimes, "courierBreakdown"));
    ok &= check("p = 0,3: un solo dueño por pedido en todo momento", oneOwner(sometimes));
    ok &= check("p = 0,3: created = delivered + rejected + pending, y lo entregado coincide con el log",
                sometimes.summary.created == 40 && sometimes.summary.delivered == count(sometimes, "orderDelivered"));

    // 2. Probabilidad 1: todo viaje se avería; los pedidos van de moto en moto y nadie entrega.
    const Run always = simulate(smallTown(1.0, 1800, 3), 10);
    std::cout << "p = 1: " << toJson(always.summary) << "\n";
    ok &= check("p = 1: hay traspasos, un solo dueño por pedido y nada entregado",
                always.summary.reassigned > 0 && always.summary.reassigned == count(always, "orderReassigned") &&
                    oneOwner(always) && always.summary.delivered == 0);
    return ok ? 0 : 1;
}
