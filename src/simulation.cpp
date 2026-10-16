#include "simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

#include "courier.hpp"
#include "event_log.hpp"
#include "order_book.hpp"
#include "shutdown.hpp"
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

// Hilo generador: crea los pedidos y los deja en el libro. Devuelve cuántos creó.
// Cada pedido tiene su instante planificado (suma de intervalos) y se crea con ese t:
// así ninguno queda con t >= durationS*1000 aunque el hilo despierte unos ms tarde.
// Al llegar a `orders` cierra el libro (fin natural); si se cumple `durationS` o se pide
// parar, solo deja de crear: el libro lo descarta quien ordena la detención.
int generateOrders(const SimParams& p, const SimClock& clock, EventLog& log, OrderBook& book,
                   const Shutdown& shutdown)
{
    std::mt19937 rng(p.seed);  // propio de este hilo: nadie más lo usa
    const std::int64_t endMs = p.durationS * 1000;
    std::int64_t plannedMs = 0;
    for (int i = 0; i < p.orders; ++i) {
        plannedMs += nextIntervalMs(rng, p.meanIntervalMs);
        if (p.durationS > 0 && plannedMs >= endMs) {
            return i;  // el próximo pedido caería después del fin: no se crea
        }
        if (shutdown.sleepUntil(clock.realTimeAt(plannedMs))) {
            return i;  // espera interrumpible: se pidió parar
        }
        const std::string restaurant = "r" + std::to_string(rng() % p.restaurants);
        const std::string destination = "n" + std::to_string(rng() % p.nodes);
        Order order("o" + std::to_string(i), restaurant, destination, plannedMs);

        Event e;  // se registra antes de soltar el pedido: ningún evento suyo puede adelantarse
        e.t = order.createdAtMs();
        e.event = "orderCreated";
        e.order = order.id();
        e.restaurant = restaurant;
        // Comprobar y registrar en una sola operación: si no, se podría pedir parar entre
        // las dos y este orderCreated quedaría en el log después de simulationStopping.
        if (!shutdown.unlessStopped(e.t, [&log, &e] { log.write(e); })) {
            return i;
        }
        if (!book.submit(std::move(order))) {
            if (shutdown.requested()) {
                return i + 1;  // ya creado y registrado: cuenta como pendiente
            }
            throw std::logic_error("el libro solo se cierra al final o al detener");
        }
    }
    book.close();
    return p.orders;
}

// Hilo despachador (v0): reparte por turnos. Cuando el libro está cerrado y vacío,
// cierra todas las bandejas para que cada moto termine al vaciar la suya. Si se detiene,
// el libro abortado le devuelve std::nullopt y un pedido que ya no entra a una bandeja
// abortada queda pendiente.
void dispatchRoundRobin(const SimClock& clock, EventLog& log, OrderBook& book,
                        const std::vector<std::shared_ptr<Courier>>& couriers, const Shutdown& shutdown)
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
        if (!shutdown.unlessStopped(e.t, [&log, &e] { log.write(e); })) {
            break;  // se detuvo (o su t ya pasó el fin): el pedido queda pendiente
        }
        if (!courier.inbox().submit(std::move(*order))) {
            if (shutdown.requested()) {
                break;
            }
            throw std::logic_error("las bandejas solo se cierran al final o al detener");
        }
    }
    for (const auto& courier : couriers) {
        courier->inbox().close();
    }
}

}  // namespace

Summary runSimulation(const SimParams& p, std::ostream& logOut, std::ostream& out)
{
    const std::atomic<bool> neverSignalled(false);
    return runSimulation(p, logOut, out, neverSignalled);
}

Summary runSimulation(const SimParams& p, std::ostream& logOut, std::ostream& out, const std::atomic<bool>& stopSignal)
{
    EventLog log(logOut);
    OrderBook book;
    const std::int64_t endMs = p.durationS * 1000;
    Shutdown shutdown(endMs);  // niega todo evento con t >= endMs: nada queda registrado después del fin
    const SimClock clock(p.timeScale);  // t = 0

    // Hilos que siguen corriendo. Es UN valor que todos tocan: atomic, y fetch_sub (RMW)
    // para que dos hilos que terminan a la vez no pierdan un descuento (lost update).
    std::atomic<int> running(p.couriers + 2);

    // Cada moto la comparten su hilo y el despachador: shared_ptr garantiza que vive
    // mientras alguno la use.
    std::vector<std::shared_ptr<Courier>> couriers;
    for (int i = 0; i < p.couriers; ++i) {
        couriers.push_back(std::make_shared<Courier>("c" + std::to_string(i), clock, log, shutdown, p.legMs));
    }
    std::vector<std::thread> courierThreads;
    courierThreads.reserve(couriers.size());
    for (const auto& courier : couriers) {
        // La lambda captura el shared_ptr por valor: el hilo tiene su propia copia.
        courierThreads.emplace_back([courier, &running] {
            courier->run();
            running.fetch_sub(1);
        });
    }
    std::thread dispatcher([&clock, &log, &book, &couriers, &shutdown, &running] {
        dispatchRoundRobin(clock, log, book, couriers, shutdown);
        running.fetch_sub(1);
    });

    out << "Simulation has started...\n" << std::flush;

    int created = 0;  // solo lo escribe el generador; se lee después del join
    std::thread generator([&p, &clock, &log, &book, &shutdown, &created, &running] {
        created = generateOrders(p, clock, log, book, shutdown);
        running.fetch_sub(1);
    });

    // Bucle de frames (~30 por segundo): no es espera activa, el hilo duerme entre frames y
    // casi no usa CPU. El enunciado permite pausar para regular los FPS de la ventana: aquí
    // irá el dibujado. En cada frame lee la bandera de la señal (SIGINT/SIGTERM): el handler
    // no puede despertar a nadie, así que alguien que ya despierta seguido la tiene que mirar.
    // Duerme hasta el próximo frame o hasta el fin, lo que llegue antes: el fin se detecta
    // a tiempo y no hasta un frame tarde. Se compara en tiempo real (sleep_until garantiza
    // despertar en o después de `endReal`), así no hay que reintentar por redondeos.
    // El tiempo se mira ANTES que `running`: pasado el fin, a las motos se les niegan los
    // eventos y terminan solas; si se mirara `running` primero, podría llegar a 0 y se
    // saldría sin registrar simulationStopping.
    const auto endReal = clock.realTimeAt(endMs);
    bool stopped = false;
    bool bySignal = false;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (p.durationS > 0 && now >= endReal) {
            stopped = true;
            break;
        }
        if (stopSignal.load()) {
            stopped = bySignal = true;
            break;
        }
        if (running.load() == 0) {
            break;  // fin natural: se entregaron todos los pedidos antes del fin
        }
        const auto nextFrame = now + std::chrono::milliseconds(33);
        std::this_thread::sleep_until(p.durationS > 0 ? std::min(nextFrame, endReal) : nextFrame);
    }

    if (stopped) {
        // Por tiempo, t = durationS*1000: el instante en que vence la duración. Por señal, la
        // hora del frame que la vio (leer el reloj en el handler no es seguro), leída DENTRO de
        // la sección crítica y nunca menor que el último t registrado (un pedido se crea con su
        // instante planificado, que puede ir 1 ms adelante del reloj). Sin pasar del fin, por si
        // la señal llega justo en el borde.
        Event e;
        e.event = "simulationStopping";
        // Pedir parar y registrarlo en una sola operación: ningún evento puede quedar después
        // en el log (todos pasan por unlessStopped). La orden despierta a quien duerme en un
        // tramo o esperando el próximo pedido; luego se descarta lo que espera en el libro y
        // en las bandejas: todo eso queda pendiente, y cada take() bloqueado devuelve std::nullopt.
        shutdown.request([&](std::int64_t latestT) {
            e.t = bySignal ? std::max(clock.nowMs(), latestT) : endMs;
            if (p.durationS > 0) {
                e.t = std::min(e.t, endMs);
            }
            log.write(e);
        });
        book.abort();
        for (const auto& courier : couriers) {
            courier->inbox().abort();
        }
    }

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
