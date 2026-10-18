// Prueba de la estimación (ETA): cada caso se compara con la cuenta hecha tramo por tramo con los
// caminos mínimos. Sin hilos: estimateDelivery es una función pura. Sale con 0 si todo está bien.
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "courier.hpp"
#include "estimate.hpp"
#include "log_line.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido, e sin calles. r0 en b, r1 en d. 1 m por ms.
std::shared_ptr<World> makeWorld()
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}, {"e", 0.002, 0.002}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    auto w = std::make_shared<World>(World{RoadGraph(c), {}, 0, 1.0});
    w->restaurantNode.emplace("r0", *w->graph.nodeIndex("b"));
    w->restaurantNode.emplace("r1", *w->graph.nodeIndex("d"));
    return w;
}

}  // namespace

int main()
{
    bool ok = true;
    const auto world = makeWorld();
    const RoadGraph& g = world->graph;
    auto n = [&g](const char* id) { return *g.nodeIndex(id); };
    // Cuánto tarda el camino mínimo (lo mismo que hace un Trip).
    auto leg = [&g](std::size_t from, std::size_t to) {
        return static_cast<std::int64_t>(std::ceil(g.shortestPath(from, to)->meters / 1.0));
    };
    const std::int64_t now = 1000, H = kHandoverMs;
    const CourierPlan idleAtA(Trip::stay(n("a"), 0));

    // 1. Moto quieta en a; pedido de r0 (b) a c, comida lista: viaje + 30 s + viaje.
    const std::int64_t base = now + leg(n("a"), n("b")) + H + leg(n("b"), n("c"));
    ok &= check("moto quieta: ir al restaurante + entrega en el puesto + llevarlo",
                estimateDelivery(*world, idleAtA, {"r0", "c", now, false}, now) == base);

    // 2. La comida tarda 100 s: espera en el puesto hasta que esté lista.
    ok &= check("si la comida no está lista, espera: lista + 30 s + viaje",
                estimateDelivery(*world, idleAtA, {"r0", "c", now + 100000, false}, now) ==
                    now + 100000 + H + leg(n("b"), n("c")));

    // 3. Ya lleva un pedido de r1 (d) para a: primero ese (FIFO), después el nuevo.
    CourierPlan busy(Trip::stay(n("a"), 0));
    busy.orders.push_back({"r1", "a", 0, false});
    const std::int64_t afterFirst = now + leg(n("a"), n("d")) + H + leg(n("d"), n("a"));
    ok &= check("con la mochila cargada: termina lo que lleva (FIFO) y después el nuevo",
                estimateDelivery(*world, busy, {"r0", "c", now, false}, now) ==
                    afterFirst + leg(n("a"), n("b")) + H + leg(n("b"), n("c")));

    // 4. Ya lleva uno de r0 para c: el nuevo, del mismo restaurante, se recoge en la misma visita.
    CourierPlan sameShop(Trip::stay(n("a"), 0));
    sameShop.orders.push_back({"r0", "c", 0, false});
    ok &= check("mismo restaurante: una sola visita (el nuevo sale junto con el que ya llevaba)",
                estimateDelivery(*world, sameShop, {"r0", "c", now, false}, now) == base);

    // 5. Va a mitad de un tramo a→c (sale en 1000): la estimación parte de c, a la hora de llegada.
    const Trip toC(g, *g.shortestPath(n("a"), n("c")), 1000, 1.0);
    const CourierPlan travelling(toC);
    ok &= check("en viaje: parte del final del tramo actual (c, a la hora de llegada)",
                estimateDelivery(*world, travelling, {"r1", "a", now, false}, now) ==
                    toC.arrivalMs() + leg(n("c"), n("d")) + H + leg(n("d"), n("a")));

    // 6. Lleva uno YA RECOGIDO para c: primero lo entrega, después va por el nuevo (c→b da la vuelta por d y a).
    CourierPlan carrying(Trip::stay(n("a"), 0));
    carrying.orders.push_back({"r0", "c", 0, true});
    ok &= check("uno ya recogido: lo entrega primero, sin pasar por el restaurante",
                estimateDelivery(*world, carrying, {"r0", "c", now, false}, now) ==
                    now + leg(n("a"), n("c")) + leg(n("c"), n("b")) + H + leg(n("b"), n("c")));

    // 7. Una moto sin camino (en e, sin calles): la estimación FALLA con EstimateError.
    std::string message;
    try {
        estimateDelivery(*world, CourierPlan(Trip::stay(n("e"), 0)), {"r0", "c", now, false}, now);
    } catch (const EstimateError& e) {
        message = e.what();
    }
    std::cout << "moto en e -> \"" << message << "\"\n";
    ok &= check("sin camino: lanza EstimateError (no devuelve un número inventado)", message == "sin camino de e a b");

    // 8. assign anota el pedido en el plan aunque la moto todavía no lo haya tomado de la bandeja.
    const SimClock clock(1000.0);
    const Shutdown shutdown;
    std::ostringstream out;
    EventLog log(out);
    Courier courier("c0", clock, log, shutdown, 60000, world);
    Order order("o1", "r1", "a", 0, 5000);
    order.markAssigned(2000);
    const bool accepted = courier.assign(std::move(order)) == OrderBook::Submit::accepted;
    const CourierPlan seen = courier.plan();
    ok &= check("assign: el plan ya lo tiene (restaurante, destino, lista en asignado + preparación)",
                accepted && seen.orders.size() == 1 && seen.orders[0].restaurant == "r1" &&
                    seen.orders[0].destination == "a" && seen.orders[0].readyAtMs == 7000 && !seen.orders[0].picked);
    courier.close();
    return ok ? 0 : 1;
}
