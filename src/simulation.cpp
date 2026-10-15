#include "simulation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

#include "courier.hpp"
#include "event_log.hpp"
#include "order_book.hpp"
#include "sim_clock.hpp"

namespace {

// Intervalo exponencial con media `meanMs`, calculado directo del mt19937: las
// distribuciones de la std dan valores distintos en libstdc++ y libc++, y la
// secuencia de pedidos debe ser la misma para una semilla con cualquier compilador.
std::int64_t nextIntervalMs(std::mt19937& rng, std::int64_t meanMs)
{
    const double u = (static_cast<double>(rng()) + 0.5) / 4294967296.0;  // (0, 1)
    return static_cast<std::int64_t>(-static_cast<double>(meanMs) * std::log(u));
}

// Hilo generador: crea los pedidos y los deja en el libro. Al terminar, cierra el libro.
int generateOrders(const SimParams& p, const SimClock& clock, EventLog& log, OrderBook& book)
{
    std::mt19937 rng(p.seed);  // propio de este hilo: nadie más lo usa
    for (int i = 0; i < p.orders; ++i) {
        clock.sleepSim(nextIntervalMs(rng, p.meanIntervalMs));
        const std::string restaurant = "r" + std::to_string(rng() % p.restaurants);
        const std::string destination = "n" + std::to_string(rng() % p.nodes);
        Order order("o" + std::to_string(i), restaurant, destination, clock.nowMs());

        Event e;  // se registra antes de soltar el pedido: ningún evento suyo puede adelantarse
        e.t = order.createdAtMs();
        e.event = "orderCreated";
        e.order = order.id();
        e.restaurant = restaurant;
        log.write(e);
        if (!book.submit(std::move(order))) {
            throw std::logic_error("el libro solo lo cierra el generador, al final");
        }
    }
    book.close();
    return p.orders;
}

// Hilo despachador (v0): reparte por turnos. Cuando el libro está cerrado y vacío,
// cierra todas las bandejas para que cada moto termine al vaciar la suya.
void dispatchRoundRobin(const SimClock& clock, EventLog& log, OrderBook& book,
                        const std::vector<std::shared_ptr<Courier>>& couriers)
{
    std::size_t next = 0;
    while (std::optional<Order> order = book.take()) {
        Courier& courier = *couriers[next];
        next = (next + 1) % couriers.size();

        Event e;
        e.t = clock.nowMs();
        e.event = "orderAssigned";
        e.order = order->id();
        e.courier = courier.id();
        log.write(e);
        if (!courier.inbox().submit(std::move(*order))) {
            throw std::logic_error("las bandejas solo las cierra el despachador, al final");
        }
    }
    for (const auto& courier : couriers) {
        courier->inbox().close();
    }
}

}  // namespace

Summary runSimulation(const SimParams& p, std::ostream& logOut, std::ostream& out)
{
    EventLog log(logOut);
    OrderBook book;
    const SimClock clock(p.timeScale);  // t = 0

    // Cada moto la comparten su hilo y el despachador: shared_ptr garantiza que vive
    // mientras alguno la use.
    std::vector<std::shared_ptr<Courier>> couriers;
    for (int i = 0; i < p.couriers; ++i) {
        couriers.push_back(std::make_shared<Courier>("c" + std::to_string(i), clock, log, p.legMs));
    }
    std::vector<std::thread> courierThreads;
    courierThreads.reserve(couriers.size());
    for (const auto& courier : couriers) {
        courierThreads.emplace_back(&Courier::run, courier);  // el hilo guarda su copia del shared_ptr
    }
    // std::thread copia sus argumentos: std::ref/std::cref hacen que el despachador use
    // el libro, el registro, el reloj y las motos reales, que viven hasta después del join.
    std::thread dispatcher(dispatchRoundRobin, std::cref(clock), std::ref(log), std::ref(book), std::cref(couriers));

    out << "Simulation has started...\n" << std::flush;

    int created = 0;  // solo lo escribe el generador; se lee después del join
    std::thread generator([&p, &clock, &log, &book, &created] { created = generateOrders(p, clock, log, book); });

    generator.join();
    dispatcher.join();
    for (std::thread& t : courierThreads) {
        t.join();
    }

    // Después de los join: cada moto contó sus entregas sola; aquí se suman una vez.
    Summary s;
    s.created = created;
    std::int64_t total = 0;
    for (const auto& courier : couriers) {
        s.delivered += courier->delivered();
        total += courier->totalDeliveryMs();
        s.maxDeliveryMs = std::max(s.maxDeliveryMs, courier->maxDeliveryMs());
        s.deliveredPerCourier.emplace_back(courier->id(), courier->delivered());
    }
    s.avgDeliveryMs = s.delivered > 0 ? total / s.delivered : 0;
    return s;
}

std::string toJson(const Summary& s)
{
    std::string json = "{\"created\": " + std::to_string(s.created) +
                       ", \"delivered\": " + std::to_string(s.delivered) +
                       ", \"rejected\": 0, \"pending\": " + std::to_string(s.created - s.delivered) +
                       ", \"reassigned\": 0, \"breakdowns\": 0, \"avgDeliveryMs\": " + std::to_string(s.avgDeliveryMs) +
                       ", \"maxDeliveryMs\": " + std::to_string(s.maxDeliveryMs) + ", \"deliveredPerCourier\": {";
    for (std::size_t i = 0; i < s.deliveredPerCourier.size(); ++i) {
        json += (i > 0 ? ", \"" : "\"") + s.deliveredPerCourier[i].first + "\": " +
                std::to_string(s.deliveredPerCourier[i].second);
    }
    return json + "}}";
}
