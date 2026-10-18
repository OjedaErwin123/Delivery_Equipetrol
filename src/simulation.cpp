#include "simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

#include "courier.hpp"
#include "estimate.hpp"
#include "event_log.hpp"
#include "fleet.hpp"
#include "order_book.hpp"
#include "road_graph.hpp"
#include "shutdown.hpp"
#include "sim_clock.hpp"
#include "thread_pool.hpp"

namespace {

// Intervalo exponencial con media `meanMs`, calculado directo del mt19937: las
// distribuciones de la std dan valores distintos en libstdc++ y libc++, y la
// secuencia de pedidos debe ser la misma para una semilla con cualquier compilador.
std::int64_t nextIntervalMs(std::mt19937& rng, double meanMs)
{
    const double u = (static_cast<double>(rng()) + 0.5) / 4294967296.0;  // (0, 1)
    return static_cast<std::int64_t>(-meanMs * std::log(u));
}

// Entero uniforme en [min, max], directo del mt19937 (misma secuencia con cualquier compilador).
std::int64_t uniformMs(std::mt19937& rng, std::int64_t min, std::int64_t max)
{
    const std::uint64_t span = static_cast<std::uint64_t>(max - min) + 1;
    const std::uint64_t draw = (static_cast<std::uint64_t>(rng()) << 32) | rng();
    return min + static_cast<std::int64_t>(draw % span);
}

struct Generated {
    int created = 0;
    int rejected = 0;
};

// Registra un evento de pedido (orderCreated u orderRejected) salvo que la simulación ya haya
// terminado: comprobar y registrar en una sola operación (ver Shutdown::unlessStopped).
bool logOrderEvent(EventLog& log, const Shutdown& shutdown, const char* event, const Order& order,
                   const char* reason, std::int64_t t)
{
    Event e;
    e.t = t;
    e.event = event;
    e.order = order.id();
    if (reason == nullptr) {
        e.restaurant = order.restaurant();
    } else {
        e.reason = reason;
    }
    return shutdown.unlessStopped(e.t, [&log, &e] { log.write(e); });
}

// Hilo generador: crea los pedidos y los deja en el libro. Devuelve cuántos creó y cuántos
// rechazó. Cada LLEGADA trae entre 1 y burstMax pedidos en el mismo instante (una ráfaga), y
// las llegadas se separan en promedio meanIntervalMs * (1 + burstMax) / 2: así el tiempo medio
// entre dos pedidos sigue siendo meanIntervalMs, como dice la configuración. Cada pedido de la
// ráfaga elige su restaurante y su destino al azar, independiente de los demás.
// Cada llegada tiene su instante planificado (suma de intervalos) y sus pedidos se crean con
// ese t: así ninguno queda con t >= durationS*1000 aunque el hilo despierte unos ms tarde.
// Si hay tope (`orders` > 0), al llegar a él cierra el libro (fin natural); si se cumple
// `durationS` o se pide parar, solo deja de crear: el libro lo descarta quien ordena la detención.
Generated generateOrders(const SimParams& p, const SimClock& clock, EventLog& log, OrderBook& book,
                         const Shutdown& shutdown)
{
    std::mt19937 rng(p.seed);  // propio de este hilo: nadie más lo usa
    const double meanArrivalMs = static_cast<double>(p.meanIntervalMs) * (1.0 + p.burstMax) / 2.0;
    const std::int64_t endMs = p.durationS * 1000;
    std::int64_t plannedMs = 0;
    Generated g;
    for (;;) {
        plannedMs += nextIntervalMs(rng, meanArrivalMs);
        if (p.durationS > 0 && plannedMs >= endMs) {
            return g;  // la próxima llegada caería después del fin: no se crea
        }
        if (shutdown.sleepUntil(clock.realTimeAt(plannedMs))) {
            return g;  // espera interrumpible: se pidió parar
        }
        const int burst = 1 + static_cast<int>(rng() % static_cast<std::uint32_t>(p.burstMax));
        for (int k = 0; k < burst; ++k) {
            const std::size_t r = rng() % p.restaurants.size();
            const std::size_t d = rng() % p.destinations.size();
            // La preparación se sortea aquí (uniforme en el rango del restaurante): es parte de la
            // secuencia reproducible por semilla.
            const std::int64_t prepMs =
                p.kitchens.empty() ? 0 : uniformMs(rng, p.kitchens[r].prepMinMs, p.kitchens[r].prepMaxMs);
            Order order("o" + std::to_string(g.created), p.restaurants[r], p.destinations[d], plannedMs, prepMs);

            // Se registra antes de soltar el pedido: ningún evento suyo puede adelantarse.
            if (!logOrderEvent(log, shutdown, "orderCreated", order, nullptr, order.createdAtMs())) {
                return g;
            }
            ++g.created;
            // El motivo del rechazo, si lo hay. unreachable se mira antes que queueFull: es una
            // propiedad fija del pedido (no depende de cuánto ocupa el libro en este instante),
            // así el motivo es el mismo en cada corrida.
            const char* rejectedFor = nullptr;
            if (!p.reachable.empty() && !p.reachable[r][d]) {
                rejectedFor = "unreachable";
            } else {
                switch (book.submit(std::move(order))) {
                case OrderBook::Submit::accepted:
                    break;
                case OrderBook::Submit::full:
                    rejectedFor = "queueFull";  // el libro tiene maxPending esperando
                    break;
                case OrderBook::Submit::closed:
                    if (shutdown.requested()) {
                        return g;  // ya creado y registrado: cuenta como pendiente
                    }
                    throw std::logic_error("el libro solo se cierra al final o al detener");
                }
            }
            if (rejectedFor != nullptr) {
                // Rechazado en el mismo instante. El pedido no entró al libro (submit no lo
                // consumió); si ya se detuvo, no se registra y queda pendiente.
                if (!logOrderEvent(log, shutdown, "orderRejected", order, rejectedFor, order.createdAtMs())) {
                    return g;
                }
                ++g.rejected;
            }
            if (p.orders > 0 && g.created == p.orders) {
                book.close();  // tope de las pruebas: no llegarán más
                return g;
            }
        }
    }
}

// Hilo despachador (v0): reparte por turnos entre las motos con lugar en la mochila (la flota
// elige y reserva el cupo en una sola operación; si todas están llenas, duerme hasta que alguna
// entregue: mientras tanto los pedidos esperan en el libro y, si se llena, salen queueFull). Cuando el libro está cerrado y vacío,
// cierra todas las bandejas para que cada moto termine al vaciar la suya. Si se detiene,
// el libro abortado le devuelve std::nullopt y un pedido que ya no entra a una bandeja
// abortada queda pendiente.
void dispatchRoundRobin(const SimClock& clock, EventLog& log, OrderBook& book,
                        const std::vector<std::shared_ptr<Courier>>& couriers, Fleet& fleet, const Shutdown& shutdown)
{
    std::size_t next = 0;
    while (std::optional<Order> order = book.take()) {
        const std::optional<std::size_t> chosen = fleet.reserve(next);
        if (!chosen) {
            break;  // detención mientras esperaba una moto con lugar: el pedido queda pendiente
        }
        Courier& courier = *couriers[*chosen];
        next = (*chosen + 1) % couriers.size();

        Event e;
        e.t = clock.nowMs();
        e.event = "orderAssigned";
        e.order = order->id();
        e.courier = courier.id();
        order->markAssigned(e.t);  // la cocina empieza ahora
        if (!shutdown.unlessStopped(e.t, [&log, &e] { log.write(e); })) {
            break;  // se detuvo (o su t ya pasó el fin): el pedido queda pendiente
        }
        if (courier.assign(std::move(*order)) != OrderBook::Submit::accepted) {
            if (shutdown.requested()) {
                break;  // bandeja abortada: el pedido queda pendiente
            }
            throw std::logic_error("las bandejas no tienen límite y solo se cierran al final o al detener");
        }
    }
    for (const auto& courier : couriers) {
        courier->close();
    }
}

// Asignar `order` a `courier` (ya con su cupo reservado): la cocina empieza ahora. false si se
// detuvo (el pedido queda pendiente).
bool assignTo(Courier& courier, Order&& order, const SimClock& clock, EventLog& log, const Shutdown& shutdown)
{
    Event e;
    e.t = clock.nowMs();
    e.event = "orderAssigned";
    e.order = order.id();
    e.courier = courier.id();
    order.markAssigned(e.t);
    if (!shutdown.unlessStopped(e.t, [&log, &e] { log.write(e); })) {
        return false;
    }
    return courier.assign(std::move(order)) == OrderBook::Submit::accepted;  // falla solo si se detiene
}

// Hilo despachador (con mapa): para cada pedido, en orden FIFO, estima con TODAS las motos con lugar
// a la vez (una tarea por moto en el pool), espera las estimaciones hasta quoteTimeoutMs reales
// (las tardías se ignoran; las que fallan se reportan en stderr y se descartan), elige la de menor
// ETA (empate: la de menor índice) y revalida reservando el cupo de ESA moto (si se llenó entre
// estimar y asignar, prueba la siguiente). Si la ronda no sirve, duerme hasta que alguna moto
// libere un cupo o hasta el vencimiento; vencido (creación + acceptTimeoutMs), noCourier.
// Las estimaciones trabajan sobre copias (plan de la moto, World de solo lectura): sin locks.
// Devuelve cuántos rechazó por noCourier.
int dispatchByEstimate(const SimParams& p, const SimClock& clock, EventLog& log, OrderBook& book,
                       const std::vector<std::shared_ptr<Courier>>& couriers, Fleet& fleet, ThreadPool& pool,
                       const Shutdown& shutdown)
{
    int noCourier = 0;
    while (std::optional<Order> order = book.take()) {
        const std::int64_t deadline = order->createdAtMs() + p.acceptTimeoutMs;
        bool assigned = false;
        for (;;) {
            const std::uint64_t seen = fleet.version();  // antes de estimar: un cupo liberado durante la ronda no se pierde
            const std::int64_t now = clock.nowMs();
            if (now >= deadline) {
                break;
            }
            const std::vector<std::size_t> candidates = fleet.withSpace();
            const PlannedOrder planned{order->restaurant(), order->destination(), now + order->prepMs(), false};
            std::vector<std::future<std::int64_t>> etas;
            for (std::size_t i : candidates) {
                // Todo por valor: la tarea puede terminar tarde, cuando el despachador ya siguió.
                etas.push_back(pool.submit([world = p.world, plan = couriers[i]->plan(), planned, now] {
                    return estimateDelivery(*world, plan, planned, now);
                }));
            }
            const auto quoteDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p.quoteTimeoutMs);
            std::vector<std::pair<std::int64_t, std::size_t>> ranked;  // (ETA, moto)
            for (std::size_t k = 0; k < etas.size(); ++k) {
                if (etas[k].wait_until(quoteDeadline) != std::future_status::ready) {
                    continue;  // tardía: se ignora (termina en el pool y nadie la lee)
                }
                try {
                    ranked.emplace_back(etas[k].get(), candidates[k]);
                } catch (const std::exception& error) {  // el error cruzó de hilo por el future
                    std::cerr << "delivery_sim: estimación fallida (pedido " << order->id() << ", moto "
                              << couriers[candidates[k]]->id() << "): " << error.what() << "\n";
                }
            }
            std::sort(ranked.begin(), ranked.end());
            for (const auto& choice : ranked) {
                if (fleet.tryReserve(choice.second)) {  // revalidar (check-then-act)
                    if (!assignTo(*couriers[choice.second], std::move(*order), clock, log, shutdown)) {
                        return noCourier;  // se detuvo
                    }
                    assigned = true;
                    break;
                }
            }
            if (assigned) {
                break;
            }
            // Sin estimación usable o sin motos con lugar: dormir (sin CPU) hasta un cambio o el vencimiento.
            if (!fleet.waitForChange(seen, clock.realTimeAt(deadline))) {
                return noCourier;  // detención
            }
        }
        if (!assigned) {
            if (!logOrderEvent(log, shutdown, "orderRejected", *order, "noCourier", clock.nowMs())) {
                return noCourier;
            }
            ++noCourier;
        }
    }
    for (const auto& courier : couriers) {
        courier->close();
    }
    return noCourier;
}

}  // namespace

SimParams paramsFromConfig(const Config& c)
{
    SimParams p;
    p.couriers = static_cast<int>(c.couriers);
    p.orders = 0;  // los pedidos se crean de forma continua durante toda la ejecución
    p.meanIntervalMs = c.meanIntervalMs;
    p.burstMax = static_cast<int>(c.burstMax);
    p.bagCapacity = static_cast<std::size_t>(c.bagCapacity);
    p.quoteTimeoutMs = c.quoteTimeoutMs;
    p.acceptTimeoutMs = c.acceptTimeoutMs;
    p.maxPending = static_cast<std::size_t>(c.maxPending);  // validado >= 0; 0 rechaza todo
    p.timeScale = c.timeScale;
    p.seed = static_cast<std::uint32_t>(c.seed);
    p.breakdownProbability = c.breakdownProbability;
    p.durationS = c.durationS;

    // Destino (decisión "C"): un nodo de la red que no sea de un restaurante. El formato no
    // tiene casas, así que el punto de entrega lo elige la simulación entre esos nodos.
    std::set<std::string> restaurantNodes;
    p.restaurants.clear();
    for (const RestaurantConfig& r : c.restaurants) {
        p.restaurants.push_back(r.id);
        p.kitchens.push_back({static_cast<std::size_t>(r.pickupSlots), r.prepMinMs, r.prepMaxMs});
        restaurantNodes.insert(r.node);
    }
    p.destinations.clear();
    for (const NodeConfig& n : c.nodes) {
        if (restaurantNodes.count(n.id) == 0) {
            p.destinations.push_back(n.id);
        }
    }
    // Una configuración válida puede tener solo nodos de restaurantes (p. ej. un nodo y un
    // restaurante): rechazarla contradiría "cualquier archivo válido se ejecuta". Entonces
    // cualquier nodo sirve de destino.
    if (p.destinations.empty()) {
        for (const NodeConfig& n : c.nodes) {
            p.destinations.push_back(n.id);
        }
    }

    // El mapa que comparten las motos: se arma una vez aquí y después nadie lo modifica.
    auto world = std::make_shared<World>(World{RoadGraph(c), {}, 0, c.speedKmh / 3600.0});  // km/h -> m/ms
    const RoadGraph& graph = world->graph;
    world->startNode = *graph.nodeIndex(c.startNode);
    for (const RestaurantConfig& r : c.restaurants) {
        world->restaurantNode.emplace(r.id, *graph.nodeIndex(r.node));
    }

    // unreachable: sin camino dirigido del nodo del restaurante al destino. El grafo ya precalculó
    // las distancias desde cada restaurante (un Dijkstra por restaurante); aquí solo se consulta.
    p.reachable.assign(p.restaurants.size(), std::vector<bool>(p.destinations.size()));
    for (std::size_t r = 0; r < p.restaurants.size(); ++r) {
        for (std::size_t d = 0; d < p.destinations.size(); ++d) {
            p.reachable[r][d] = graph.reachableFromRestaurant(r, *graph.nodeIndex(p.destinations[d]));
        }
    }
    p.world = std::move(world);
    return p;
}

Summary runSimulation(const SimParams& p, std::ostream& logOut, std::ostream& out)
{
    const std::atomic<bool> neverSignalled(false);
    return runSimulation(p, logOut, out, neverSignalled);
}

Summary runSimulation(const SimParams& p, std::ostream& logOut, std::ostream& out, const std::atomic<bool>& stopSignal)
{
    EventLog log(logOut);
    OrderBook book(p.maxPending);
    const std::int64_t endMs = p.durationS * 1000;
    Shutdown shutdown(endMs);  // niega todo evento con t >= endMs: nada queda registrado después del fin
    const SimClock clock(p.timeScale);  // t = 0

    // Hilos que siguen corriendo. Es UN valor que todos tocan: atomic, y fetch_sub (RMW)
    // para que dos hilos que terminan a la vez no pierdan un descuento (lost update).
    std::atomic<int> running(p.couriers + 2);

    // Cada moto la comparten su hilo y el despachador: shared_ptr garantiza que vive
    // mientras alguno la use.
    // Los restaurantes con sus puestos (monitores que comparten las motos que llegan).
    std::shared_ptr<const Restaurants> restaurants;
    if (!p.kitchens.empty()) {
        auto all = std::make_shared<Restaurants>();
        for (std::size_t r = 0; r < p.restaurants.size(); ++r) {
            all->emplace(p.restaurants[r], std::make_shared<Restaurant>(p.restaurants[r], p.kitchens[r].pickupSlots));
        }
        restaurants = std::move(all);
    }

    // El cupo de las mochilas: lo reserva el despachador y lo libera cada moto.
    auto fleet = std::make_shared<Fleet>(static_cast<std::size_t>(p.couriers),
                                         p.bagCapacity.value_or(std::numeric_limits<std::size_t>::max()));

    std::vector<std::shared_ptr<Courier>> couriers;
    for (int i = 0; i < p.couriers; ++i) {
        couriers.push_back(std::make_shared<Courier>("c" + std::to_string(i), clock, log, shutdown, p.legMs, p.world,
                                                     restaurants, fleet, static_cast<std::size_t>(i),
                                                     p.breakdownProbability, p.seed + static_cast<std::uint32_t>(i)));
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
    // Con mapa, el despachador estima en un thread pool de tantos hilos como núcleos (01: la
    // decisión que depende del hardware). unique_ptr: el pool tiene un solo dueño y se destruye
    // (join de sus hilos) al final de runSimulation, después del join del despachador.
    std::unique_ptr<ThreadPool> pool;
    if (p.world) {
        pool = std::make_unique<ThreadPool>(ThreadPool::defaultSize());
    }
    int noCourier = 0;  // solo lo escribe el despachador; se lee después del join
    std::thread dispatcher([&p, &clock, &log, &book, &couriers, &fleet, &pool, &shutdown, &noCourier, &running] {
        if (pool) {
            noCourier = dispatchByEstimate(p, clock, log, book, couriers, *fleet, *pool, shutdown);
        } else {
            dispatchRoundRobin(clock, log, book, couriers, *fleet, shutdown);  // pruebas sin mapa
        }
        running.fetch_sub(1);
    });

    out << "Simulation has started...\n" << std::flush;

    Generated generated;  // solo lo escribe el generador; se lee después del join
    std::thread generator([&p, &clock, &log, &book, &shutdown, &generated, &running] {
        generated = generateOrders(p, clock, log, book, shutdown);
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
        fleet->abort();  // el despachador puede estar esperando una moto con lugar
        for (const auto& courier : couriers) {
            courier->abort();
        }
        if (restaurants) {
            for (const auto& restaurant : *restaurants) {
                restaurant.second->abort();  // despierta a las motos que hacen fila
            }
        }
    }

    generator.join();
    dispatcher.join();
    for (std::thread& t : courierThreads) {
        t.join();
    }

    // Después de los join: cada moto contó sus entregas sola; aquí se suman una vez.
    Summary s;
    s.created = generated.created;
    s.rejected = generated.rejected + noCourier;
    std::int64_t total = 0;
    for (const auto& courier : couriers) {
        s.delivered += courier->delivered();
        total += courier->totalDeliveryMs();
        s.maxDeliveryMs = std::max(s.maxDeliveryMs, courier->maxDeliveryMs());
        s.breakdowns += courier->breakdowns();
        s.deliveredPerCourier.emplace_back(courier->id(), courier->delivered());
    }
    s.avgDeliveryMs = s.delivered > 0 ? total / s.delivered : 0;
    return s;
}

std::string toJson(const Summary& s)
{
    std::string json = "{\"created\": " + std::to_string(s.created) +
                       ", \"delivered\": " + std::to_string(s.delivered) +
                       ", \"rejected\": " + std::to_string(s.rejected) +
                       ", \"pending\": " + std::to_string(s.created - s.delivered - s.rejected) +
                       ", \"reassigned\": 0, \"breakdowns\": " + std::to_string(s.breakdowns) +
                       ", \"avgDeliveryMs\": " + std::to_string(s.avgDeliveryMs) +
                       ", \"maxDeliveryMs\": " + std::to_string(s.maxDeliveryMs) + ", \"deliveredPerCourier\": {";
    for (std::size_t i = 0; i < s.deliveredPerCourier.size(); ++i) {
        json += (i > 0 ? ", \"" : "\"") + s.deliveredPerCourier[i].first + "\": " +
                std::to_string(s.deliveredPerCourier[i].second);
    }
    return json + "}}";
}
