// Prueba de la moto: corre en su propio hilo, entrega lo que recibe en su bandeja
// en orden de llegada, registra sus eventos y termina cuando le cierran la bandeja.
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

namespace {

constexpr int kOrders = 10;
constexpr std::int64_t kLegMs = 2000;  // 2 s simulados por tramo
constexpr double kTimeScale = 1000.0;  // 1 s simulado = 1 ms real: la prueba dura milisegundos

// Valor del campo de texto `key` en una línea del log ("" si no está).
std::string field(const std::string& line, const std::string& key)
{
    const std::string open = "\"" + key + "\": \"";
    const auto start = line.find(open);
    if (start == std::string::npos) {
        return "";
    }
    const auto begin = start + open.size();
    return line.substr(begin, line.find('"', begin) - begin);
}

std::int64_t timeOf(const std::string& line) { return std::stoll(line.substr(line.find(':') + 1)); }

bool check(const char* name, bool ok)
{
    std::cout << name << ": " << (ok ? "OK" : "FALLA") << "\n";
    return ok;
}

}  // namespace

int main()
{
    bool ok = true;
    const SimClock clock(kTimeScale);

    // 1. Una moto recibe pedidos mientras ya está corriendo.
    std::ostringstream out;
    EventLog log(out);
    auto courier = std::make_shared<Courier>("c0", clock, log, kLegMs);
    std::thread worker(&Courier::run, courier);  // el hilo guarda su copia del shared_ptr

    std::vector<std::int64_t> createdAt;
    for (int i = 0; i < kOrders; ++i) {
        createdAt.push_back(clock.nowMs());
        Order order("o" + std::to_string(i), "r" + std::to_string(i % 3), "n" + std::to_string(i), createdAt.back());
        if (!courier->inbox().submit(std::move(order))) {
            ok = check("bandeja abierta mientras se envían pedidos", false);
        }
    }
    courier->inbox().close();
    worker.join();

    // Cada pedido: pickupStarted, orderPickedUp, orderDelivered, en ese orden y con la
    // moto y el restaurante correctos. Los pedidos, en orden de llegada (FIFO).
    std::vector<std::string> lines;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    const char* expected[] = {"pickupStarted", "orderPickedUp", "orderDelivered"};
    bool sequence = lines.size() == 3 * kOrders;
    bool timeForward = true;
    std::int64_t total = 0;
    std::int64_t previous = 0;
    for (std::size_t k = 0; sequence && k < lines.size(); ++k) {
        const int i = static_cast<int>(k / 3);
        const std::string& line = lines[k];
        sequence = field(line, "event") == expected[k % 3] && field(line, "order") == "o" + std::to_string(i) &&
                   field(line, "courier") == "c0" &&
                   field(line, "restaurant") == (k % 3 == 2 ? "" : "r" + std::to_string(i % 3));
        timeForward = timeForward && timeOf(line) >= previous;
        previous = timeOf(line);
        if (k % 3 == 2) {
            total += timeOf(line) - createdAt[i];
        }
    }

    std::cout << "eventos registrados: " << lines.size() << " de " << 3 * kOrders << "\n";
    ok &= check("tres eventos por pedido, en orden, pedidos FIFO", sequence);
    ok &= check("el tiempo nunca retrocede", timeForward);
    ok &= check("entregas contadas", courier->delivered() == kOrders);
    ok &= check("tiempo de entrega igual al del log", courier->totalDeliveryMs() == total);
    // Cada entrega incluye dos tramos; -1 por el truncado a milisegundos enteros del reloj.
    ok &= check("cada tramo dura lo simulado", courier->maxDeliveryMs() >= 2 * kLegMs - 1);

    // 2. Una moto sin pedidos termina cuando se cierra su bandeja (si no, el join se colgaría).
    std::ostringstream idleOut;
    EventLog idleLog(idleOut);
    auto idle = std::make_shared<Courier>("c1", clock, idleLog, kLegMs);
    std::thread idleWorker(&Courier::run, idle);
    idle->inbox().close();
    idleWorker.join();
    ok &= check("moto sin pedidos termina al cerrar la bandeja", idle->delivered() == 0 && idleOut.str().empty());

    return ok ? 0 : 1;
}
