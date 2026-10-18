// Prueba de la mochila: una moto con varios pedidos asignados recoge en UNA visita todos los de un
// mismo restaurante (un ticket), espera la comida del último y entrega en orden de asignación
// (FIFO), viajando por las calles. Sale con 0 si todo está bien.
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"

int main()
{
    bool ok = true;

    // a→b→c de sentido único, a–d–c de doble sentido. r0 está en b y r1 en d; la moto sale de a.
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    auto world = std::make_shared<World>(World{RoadGraph(c), {}, 0, 1.0});  // 1 m por ms simulado
    world->startNode = *world->graph.nodeIndex("a");
    world->restaurantNode.emplace("r0", *world->graph.nodeIndex("b"));
    world->restaurantNode.emplace("r1", *world->graph.nodeIndex("d"));
    auto restaurants = std::make_shared<Restaurants>();
    restaurants->emplace("r0", std::make_shared<Restaurant>("r0", 1));
    restaurants->emplace("r1", std::make_shared<Restaurant>("r1", 1));

    const SimClock clock(1000.0);  // 1 s simulado = 1 ms real
    const Shutdown shutdown;
    std::ostringstream out;
    EventLog log(out);
    auto courier = std::make_shared<Courier>("c0", clock, log, shutdown, 60000, world, restaurants);

    // Tres pedidos asignados antes de que la moto salga: o0 (r0), o1 (r1), o2 (r0). La comida de
    // o2 tarda 20 s: en la visita a r0 la moto espera la del último.
    const std::int64_t assignedAt = clock.nowMs();
    const char* restaurant[] = {"r0", "r1", "r0"};
    const char* destination[] = {"c", "a", "c"};
    const std::int64_t prep[] = {0, 0, 20000};
    for (int i = 0; i < 3; ++i) {
        Order order("o" + std::to_string(i), restaurant[i], destination[i], assignedAt, prep[i]);
        order.markAssigned(assignedAt);
        if (courier->assign(std::move(order)) != OrderBook::Submit::accepted) {
            ok = check("bandeja abierta", false);
        }
    }
    courier->inbox().close();
    std::thread worker(&Courier::run, courier);
    worker.join();

    std::map<std::string, std::map<std::string, std::int64_t>> t;  // pedido -> evento -> t
    std::vector<std::string> deliveries;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        t[field(line, "order")][field(line, "event")] = timeOf(line);
        if (field(line, "event") == "orderDelivered") {
            deliveries.push_back(field(line, "order"));
        }
    }
    std::cout << "recogidas: o0 t=" << t["o0"]["pickupStarted"] << ", o2 t=" << t["o2"]["pickupStarted"]
              << ", o1 t=" << t["o1"]["pickupStarted"] << "; entregas:";
    for (const auto& d : deliveries) {
        std::cout << " " << d;
    }
    std::cout << "\n";

    ok &= check("una sola visita a r0: o0 y o2 se empiezan a recoger en el mismo instante",
                t["o0"]["pickupStarted"] == t["o2"]["pickupStarted"] && t["o0"]["pickupStarted"] > 0);
    ok &= check("y se recogen juntos, después de la comida del último (o2: 20 s) y los 30 s de entrega",
                t["o0"]["orderPickedUp"] == t["o2"]["orderPickedUp"] &&
                    t["o2"]["orderPickedUp"] >= assignedAt + 20000 + kHandoverMs - 1);
    ok &= check("o1 se recoge en otra visita (r1), después", t["o1"]["pickupStarted"] > t["o0"]["orderPickedUp"]);
    ok &= check("entregas en orden de asignación: o0, o1, o2",
                deliveries == std::vector<std::string>{"o0", "o1", "o2"});
    ok &= check("los tres entregados, y la moto quieta donde entregó el último (c)",
                courier->delivered() == 3 && courier->trip().to() == *world->graph.nodeIndex("c"));
    return ok ? 0 : 1;
}
