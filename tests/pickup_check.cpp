// Prueba de la recogida en el restaurante con motos reales: nunca más pedidos recogiéndose a la
// vez que puestos (mirando solo el log: de pickupStarted a orderPickedUp), la moto espera la
// comida y la entrega dura kHandoverMs, y la detención despierta a las motos que hacen fila.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"

namespace {

constexpr double kTimeScale = 1000.0;  // 1 s simulado = 1 ms real: 30 s de entrega = 30 ms reales

struct Pickup {
    std::int64_t started = -1, picked = -1;
};

// Corre `count` motos (sin mapa, tramos de 1 s) con un pedido cada una al restaurante r0 de
// `slots` puestos. Devuelve la recogida de cada pedido según el log.
std::map<std::string, Pickup> run(int count, std::size_t slots, std::int64_t prepMs, std::int64_t& assignedAt)
{
    const SimClock clock(kTimeScale);
    const Shutdown shutdown;
    std::ostringstream out;
    EventLog log(out);
    auto restaurants = std::make_shared<Restaurants>();
    restaurants->emplace("r0", std::make_shared<Restaurant>("r0", slots));
    std::vector<std::shared_ptr<Courier>> couriers;
    std::vector<std::thread> threads;
    assignedAt = clock.nowMs();
    for (int i = 0; i < count; ++i) {
        couriers.push_back(std::make_shared<Courier>("c" + std::to_string(i), clock, log, shutdown, 1000, nullptr,
                                                     restaurants));
        threads.emplace_back(&Courier::run, couriers.back());
        Order order("o" + std::to_string(i), "r0", "n1", assignedAt, prepMs);
        order.markAssigned(assignedAt);
        if (couriers.back()->assign(std::move(order)) != OrderBook::Submit::accepted) {
            std::cout << "bandeja cerrada\n";
        }
        couriers.back()->close();
    }
    for (std::thread& t : threads) {
        t.join();
    }
    std::map<std::string, Pickup> pickups;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        if (field(line, "event") == "pickupStarted") pickups[field(line, "order")].started = timeOf(line);
        if (field(line, "event") == "orderPickedUp") pickups[field(line, "order")].picked = timeOf(line);
    }
    return pickups;
}

// Máximo de recogidas en curso a la vez, según los intervalos [pickupStarted, orderPickedUp).
int maxAtOnce(const std::map<std::string, Pickup>& pickups)
{
    std::vector<std::pair<std::int64_t, int>> marks;
    for (const auto& p : pickups) {
        marks.emplace_back(p.second.started, +1);
        marks.emplace_back(p.second.picked, -1);
    }
    std::sort(marks.begin(), marks.end());  // a igual t, el -1 va antes: termina uno, empieza otro
    int now = 0, most = 0;
    for (const auto& m : marks) {
        now += m.second;
        most = std::max(most, now);
    }
    return most;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Un puesto, 3 motos, comida que tarda 5 s: de a una, cada una con sus 30 s de entrega, y la
    //    primera no recoge antes de que la comida esté lista.
    std::int64_t assignedAt = 0;
    const auto one = run(3, 1, 5000, assignedAt);
    bool complete = one.size() == 3, handover = true, waitedFood = true;
    for (const auto& p : one) {
        complete = complete && p.second.started >= 0 && p.second.picked >= 0;
        handover = handover && p.second.picked - p.second.started >= kHandoverMs - 1;  // -1: truncado del reloj
        waitedFood = waitedFood && p.second.picked >= assignedAt + 5000 + kHandoverMs - 1;
    }
    std::cout << "1 puesto: máximo recogiendo a la vez = " << maxAtOnce(one) << "\n";
    ok &= check("1 puesto: los 3 pedidos recogidos", complete);
    ok &= check("1 puesto: nunca más de 1 recogiendo a la vez (según el log)", maxAtOnce(one) == 1);
    ok &= check("la entrega en el puesto dura kHandoverMs", handover);
    ok &= check("nadie recoge antes de que la comida esté lista + la entrega", waitedFood);

    // 2. Dos puestos, 4 motos: nunca más de 2 a la vez.
    const auto two = run(4, 2, 0, assignedAt);
    std::cout << "2 puestos: máximo recogiendo a la vez = " << maxAtOnce(two) << "\n";
    ok &= check("2 puestos: los 4 recogidos y nunca más de 2 a la vez", two.size() == 4 && maxAtOnce(two) <= 2);

    // 3. Detención con motos en la fila: abort del restaurante + orden de parar. Todas terminan.
    const SimClock clock(kTimeScale);
    Shutdown shutdown;
    std::ostringstream out;
    EventLog log(out);
    auto restaurants = std::make_shared<Restaurants>();
    auto r0 = std::make_shared<Restaurant>("r0", 1);
    restaurants->emplace("r0", r0);
    std::vector<std::shared_ptr<Courier>> couriers;
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        couriers.push_back(std::make_shared<Courier>("c" + std::to_string(i), clock, log, shutdown, 0, nullptr,
                                                     restaurants));
        threads.emplace_back(&Courier::run, couriers.back());
        Order order("o" + std::to_string(i), "r0", "n1", 0, 3600000);  // la comida tarda 1 h simulada
        order.markAssigned(0);
        if (couriers.back()->assign(std::move(order)) != OrderBook::Submit::accepted) {
            std::cout << "bandeja cerrada\n";
        }
        couriers.back()->close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // representa "en algún momento" (no coordina)
    const auto stopAt = std::chrono::steady_clock::now();
    shutdown.request([](std::int64_t) {});
    r0->abort();
    for (std::thread& t : threads) {
        t.join();  // si la fila no despertara, esto se colgaría
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopAt);
    int delivered = 0;
    for (const auto& c : couriers) {
        delivered += c->delivered();
    }
    ok &= check("detención: las motos en la fila y la del puesto terminan enseguida, sin entregar",
                ms.count() < 1000 && delivered == 0 && r0->inUse() == 0);
    return ok ? 0 : 1;
}
