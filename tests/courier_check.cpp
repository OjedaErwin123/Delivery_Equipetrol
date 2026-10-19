// Prueba de la moto: corre en su propio hilo, entrega lo que recibe en su bandeja
// en orden de asignación, registra sus eventos y termina cuando le cierran la bandeja.
// Sale con 0 si todo está bien.
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "courier.hpp"
#include "log_line.hpp"

namespace {

constexpr int kOrders = 10;
constexpr std::int64_t kLegMs = 2000;  // 2 s simulados por tramo
constexpr double kTimeScale = 1000.0;  // 1 s simulado = 1 ms real: la prueba dura milisegundos

}  // namespace

int main()
{
    bool ok = true;
    const SimClock clock(kTimeScale);
    const Shutdown shutdown;  // nunca se pide parar: la moto termina al cerrar su bandeja

    // 1. Una moto recibe pedidos mientras ya está corriendo.
    std::ostringstream out;
    EventLog log(out);
    auto courier = std::make_shared<Courier>("c0", clock, log, shutdown, kLegMs);
    std::thread worker(&Courier::run, courier);  // el hilo guarda su copia del shared_ptr

    std::vector<std::int64_t> createdAt;
    for (int i = 0; i < kOrders; ++i) {
        createdAt.push_back(clock.nowMs());
        Order order("o" + std::to_string(i), "r" + std::to_string(i % 3), "n" + std::to_string(i), createdAt.back());
        if (courier->assign(std::move(order)) != OrderBook::Submit::accepted) {
            ok = check("bandeja abierta mientras se envían pedidos", false);
        }
    }
    courier->close();
    worker.join();

    // Cada pedido: pickupStarted, orderPickedUp, orderDelivered, en ese orden y con la moto y el
    // restaurante correctos. Los eventos de pedidos distintos pueden intercalarse (la moto recoge
    // juntos los de un mismo restaurante), pero las ENTREGAS salen en orden de asignación (FIFO).
    std::vector<std::vector<std::string>> perOrder(kOrders);  // eventos de cada pedido, en orden
    std::vector<int> deliveredOrder;
    std::int64_t total = 0;
    std::int64_t previous = 0;
    bool timeForward = true;
    bool fields = true;
    int lines = 0;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line); ++lines) {
        const int i = std::stoi(field(line, "order").substr(1));
        const std::string event = field(line, "event");
        perOrder[i].push_back(event);
        fields = fields && field(line, "courier") == "c0" &&
                 field(line, "restaurant") == (event == "orderDelivered" ? "" : "r" + std::to_string(i % 3));
        timeForward = timeForward && timeOf(line) >= previous;
        previous = timeOf(line);
        if (event == "orderDelivered") {
            deliveredOrder.push_back(i);
            total += timeOf(line) - createdAt[i];
        }
    }
    bool sequence = fields && lines == 3 * kOrders;
    for (const auto& events : perOrder) {
        sequence = sequence && events == std::vector<std::string>{"pickupStarted", "orderPickedUp", "orderDelivered"};
    }
    bool fifo = static_cast<int>(deliveredOrder.size()) == kOrders;
    for (int k = 0; fifo && k < kOrders; ++k) {
        fifo = deliveredOrder[k] == k;
    }

    std::cout << "eventos registrados: " << lines << " de " << 3 * kOrders << "\n";
    ok &= check("tres eventos por pedido, en orden", sequence);
    ok &= check("entregas en orden de asignación (FIFO)", fifo);
    ok &= check("el tiempo nunca retrocede", timeForward);
    ok &= check("entregas contadas", courier->delivered() == kOrders);
    ok &= check("tiempo de entrega igual al del log", courier->totalDeliveryMs() == total);
    // Cada entrega incluye dos tramos; -1 por el truncado a milisegundos enteros del reloj.
    ok &= check("cada tramo dura lo simulado", courier->maxDeliveryMs() >= 2 * kLegMs - 1);

    // 2. Una moto sin pedidos termina cuando se cierra su bandeja (si no, el join se colgaría).
    std::ostringstream idleOut;
    EventLog idleLog(idleOut);
    auto idle = std::make_shared<Courier>("c1", clock, idleLog, shutdown, kLegMs);
    std::thread idleWorker(&Courier::run, idle);
    idle->close();
    idleWorker.join();
    ok &= check("moto sin pedidos termina al cerrar la bandeja", idle->delivered() == 0 && idleOut.str().empty());

    return ok ? 0 : 1;
}
