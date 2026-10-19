// Prueba del rescate: la estimación incluye ir a buscar la comida heredada al punto de la avería; y en simulaciones con averías, un pedido que se
// traspasa ya recogido no se entrega antes de que el receptor vaya a buscarlo (kRescueMs en el punto),
// y la moto averiada no vuelve a moverse antes de que le retiren la comida y se repare.
// Sale con 0 si todo está bien.
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido; r0 en b y r1 en d (1 m por ms simulado).
Config smallTown(double breakdownProbability, std::int64_t durationS)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 2, 0, 0}};
    c.couriers = 3;
    c.bagCapacity = 3;
    c.speedKmh = 3600;
    c.startNode = "a";
    c.meanIntervalMs = 5000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 5;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = 100000000;
    c.breakdownProbability = breakdownProbability;
    c.durationS = durationS;
    c.timeScale = 1000;
    return c;
}

}  // namespace

int main()
{
    bool ok = true;
    const RoadGraph g(smallTown(0, 0));
    const std::size_t a = *g.nodeIndex("a"), b = *g.nodeIndex("b"), d = *g.nodeIndex("d");
    const Place spot(a, b, 40);  // la comida quedó a 40 m de a sobre a→b (los viajes hasta ahí: place_check)

    // 1. La estimación con comida heredada: ir al punto + kRescueMs + llevarlo al cliente (d).
    auto world = std::make_shared<World>(World{g, {{"r0", b}, {"r1", d}}, a, 1.0});
    CourierPlan plan(Trip::stay(Place(a), 0));
    plan.orders.push_back({"r0", "d", 0, true, spot});
    const std::int64_t eta = estimateDelivery(*world, plan, {"r1", "c", 0, false}, 0);
    const std::int64_t toFood = 40 + kRescueMs;
    const std::int64_t toFirst = toFood + static_cast<std::int64_t>(std::ceil(routeTo(g, spot, d)->meters));
    const std::int64_t expectedEta = toFirst + kHandoverMs + static_cast<std::int64_t>(std::ceil(g.shortestPath(d, *g.nodeIndex("c"))->meters));
    std::cout << "estimación con rescate: " << eta << " (esperado " << expectedEta << ")\n";
    ok &= check("la estimación va a buscar la comida heredada antes de seguir", eta == expectedEta);

    // 2. Simulaciones con averías: cuándo se entrega lo traspasado ya recogido, y cuándo vuelve a
    //    moverse la moto que lo tenía.
    int rescued = 0;
    bool deliveredAfterRescue = true, ownerWaited = true;
    for (std::uint32_t seed : {5u, 6u, 7u}) {
        Config c = smallTown(0.3, 7200);
        c.seed = seed;
        SimParams p = paramsFromConfig(c);
        p.orders = 40;
        std::ostringstream logOut, out;
        runSimulation(p, logOut, out);
        std::istringstream in(logOut.str());
        std::set<std::string> picked;                            // pedidos ya recogidos
        std::map<std::string, std::int64_t> handedPicked;        // pedido recogido -> t del traspaso
        std::map<std::string, std::int64_t> ownerFree;           // moto -> antes de esto no se mueve
        for (std::string line; std::getline(in, line);) {
            const std::string event = field(line, "event"), order = field(line, "order");
            const std::string courier = field(line, "courier");
            const std::int64_t t = timeOf(line);
            if (event == "orderPickedUp") {
                picked.insert(order);
            } else if (event == "orderReassigned" && picked.count(order) && !handedPicked.count(order)) {
                handedPicked[order] = t;
                ownerFree[field(line, "from")] = t + kRescueMs + kRepairMs;
            } else if (event == "orderDelivered" && handedPicked.count(order)) {
                ++rescued;
                deliveredAfterRescue = deliveredAfterRescue && t >= handedPicked[order] + kRescueMs;
            }
            const auto free = ownerFree.find(courier);
            if (!courier.empty() && free != ownerFree.end() &&
                (event == "courierBreakdown" || event == "pickupStarted" || event == "orderDelivered")) {
                ownerWaited = ownerWaited && t >= free->second;
                ownerFree.erase(free);
            }
        }
    }
    std::cout << "pedidos rescatados y entregados: " << rescued << "\n";
    ok &= check("hubo pedidos traspasados ya recogidos que se entregaron", rescued > 0);
    ok &= check("lo traspasado ya recogido se entrega después de ir a buscarlo (>= 15 s del traspaso)",
                deliveredAfterRescue);
    ok &= check("la moto averiada no vuelve a moverse antes del rescate y la reparación", ownerWaited);
    return ok ? 0 : 1;
}
