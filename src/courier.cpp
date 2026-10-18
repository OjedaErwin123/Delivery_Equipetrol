#include "courier.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

Courier::Courier(std::string id, const SimClock& clock, EventLog& log, const Shutdown& shutdown,
                 std::int64_t legMs, std::shared_ptr<const World> world,
                 std::shared_ptr<const Restaurants> restaurants, std::shared_ptr<Fleet> fleet, std::size_t index)
    : _id(std::move(id)), _clock(clock), _log(log), _shutdown(shutdown), _legMs(legMs), _world(std::move(world)),
      _restaurants(std::move(restaurants)), _fleet(std::move(fleet)), _index(index),
      _node(_world ? _world->startNode : 0), _plan(Trip::stay(_node, 0))
{
}

void Courier::finished()
{
    if (_fleet) {
        _fleet->release(_index);
    }
}

Trip Courier::trip() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _plan.trip;
}

CourierPlan Courier::plan() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _plan;
}

OrderBook::Submit Courier::assign(Order&& order)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _plan.orders.push_back({order.restaurant(), order.destination(), order.readyAtMs(), false});
    }
    return _inbox.submit(std::move(order));  // solo falla si se está deteniendo: el plan ya no importa
}

void Courier::publish(const Trip& trip)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _plan.trip = trip;
}

void Courier::markPicked(std::size_t i)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (i < _plan.orders.size()) {
        _plan.orders[i].picked = true;
    }
}

void Courier::dropFirst()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_plan.orders.empty()) {
        _plan.orders.erase(_plan.orders.begin());
    }
}

Courier::Leg Courier::travel(const std::string& place, bool isRestaurant)
{
    if (!_world) {
        return _shutdown.sleepUntil(_clock.realTimeIn(_legMs)) ? Leg::stopped : Leg::arrived;
    }
    const std::size_t target = isRestaurant ? _world->restaurantNode.at(place) : *_world->graph.nodeIndex(place);
    const std::optional<RoadGraph::Path> path = _world->graph.shortestPath(_node, target);
    if (!path) {
        return Leg::noPath;
    }
    // Se publica el viaje una vez y se duerme hasta la llegada: la posición en cada instante
    // sale del Trip (función del tiempo), la moto no despierta para "avanzar".
    const Trip trip(_world->graph, *path, _clock.nowMs(), _world->metersPerMs);
    publish(trip);
    if (_shutdown.sleepUntil(_clock.realTimeAt(trip.arrivalMs()))) {
        return Leg::stopped;
    }
    _node = target;
    publish(Trip::stay(target, trip.arrivalMs()));
    return Leg::arrived;
}

void Courier::collect(std::deque<Carried>& bag)
{
    while (std::optional<Order> order = _inbox.tryTake()) {
        bag.push_back({std::move(*order), false});
    }
}

Courier::Leg Courier::pickUp(std::deque<Carried>& bag)
{
    const std::string restaurant = bag.front().order.restaurant();
    // Sin camino hasta el restaurante (moto atrapada, solo con mapas de evaluadores): noPath.
    const Leg leg = travel(restaurant, true);
    if (leg != Leg::arrived) {
        return leg;
    }
    collect(bag);  // lo asignado mientras viajaba también se recoge en esta visita

    // Fila (ticket) hasta tomar un puesto; nullopt = se detuvo mientras esperaba. El Slot libera el
    // puesto al destruirse (RAII), en cualquier salida. (Slot es move-only sin asignación: el
    // optional se construye directamente con el resultado.)
    std::optional<Restaurant::Slot> slot =
        _restaurants ? _restaurants->at(restaurant)->enter() : std::optional<Restaurant::Slot>();
    if (_restaurants && !slot) {
        return Leg::stopped;
    }
    // Una sola visita: todos los pedidos de la mochila de este restaurante que falta recoger.
    // Un evento negado (se pidió parar, o su t ya pasó el fin) corta: los pedidos quedan pendientes.
    std::vector<std::size_t> here;
    for (std::size_t i = 0; i < bag.size(); ++i) {
        if (!bag[i].picked && bag[i].order.restaurant() == restaurant) {
            here.push_back(i);
        }
    }
    const std::int64_t startedAt = _clock.nowMs();
    std::int64_t readyAt = startedAt;
    for (std::size_t i : here) {
        if (!logEvent("pickupStarted", bag[i].order, startedAt, true)) {
            return Leg::stopped;
        }
        readyAt = std::max(readyAt, bag[i].order.readyAtMs());
    }
    // Con el puesto tomado: espera la comida del último si no está lista, y la entrega dura kHandoverMs.
    if (_restaurants && _shutdown.sleepUntil(_clock.realTimeAt(readyAt + kHandoverMs))) {
        return Leg::stopped;
    }
    const std::int64_t pickedAt = _clock.nowMs();
    for (std::size_t i : here) {
        if (!logEvent("orderPickedUp", bag[i].order, pickedAt, true)) {
            return Leg::stopped;
        }
        bag[i].picked = true;
        markPicked(i);
    }
    return Leg::arrived;  // aquí se destruye el Slot: entra el ticket siguiente
}

void Courier::run()
{
    std::deque<Carried> bag;  // la mochila, en orden de asignación; solo la toca este hilo
    for (;;) {
        if (bag.empty()) {
            // Sin pedidos: duerme sin CPU hasta que le asignen uno o le cierren la bandeja.
            std::optional<Order> order = _inbox.take();
            if (!order) {
                return;
            }
            bag.push_back({std::move(*order), false});
        }
        collect(bag);

        // El primero de la mochila es el próximo a entregar (FIFO). Si falta recogerlo, a su restaurante.
        if (!bag.front().picked) {
            const Leg leg = pickUp(bag);
            if (leg == Leg::stopped) {
                return;
            }
            if (leg == Leg::noPath) {
                finished();  // no puede tomarlo: queda pendiente y la moto sigue con el siguiente
                bag.pop_front();
                dropFirst();
                continue;
            }
        }

        // Al cliente hay camino desde el restaurante (si no, el pedido habría salido unreachable).
        const Order& order = bag.front().order;
        const Leg toCustomer = travel(order.destination(), false);
        if (toCustomer == Leg::stopped) {
            return;
        }
        if (toCustomer == Leg::noPath) {
            finished();  // no debería pasar; si pasa, el pedido queda pendiente y la moto sigue
            bag.pop_front();
            dropFirst();
            continue;
        }
        // El mismo t va al log y a las estadísticas: el resumen debe coincidir con el log.
        const std::int64_t deliveredAt = _clock.nowMs();
        if (!logEvent("orderDelivered", order, deliveredAt, false)) {
            return;
        }
        const std::int64_t took = deliveredAt - order.createdAtMs();
        ++_delivered;
        _totalDeliveryMs += took;
        _maxDeliveryMs = std::max(_maxDeliveryMs, took);
        bag.pop_front();
        dropFirst();
        finished();  // después de registrar la entrega: la próxima asignación tiene un t posterior
    }
}

bool Courier::logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant)
{
    Event e;
    e.t = t;
    e.event = event;
    e.order = order.id();
    e.courier = _id;
    if (withRestaurant) {
        e.restaurant = order.restaurant();
    }
    return _shutdown.unlessStopped(t, [this, &e] { _log.write(e); });
}
