#include "courier.hpp"

#include <algorithm>
#include <optional>
#include <utility>

Courier::Courier(std::string id, const SimClock& clock, EventLog& log, const Shutdown& shutdown,
                 std::int64_t legMs, std::shared_ptr<const World> world,
                 std::shared_ptr<const Restaurants> restaurants)
    : _id(std::move(id)), _clock(clock), _log(log), _shutdown(shutdown), _legMs(legMs), _world(std::move(world)),
      _restaurants(std::move(restaurants)), _node(_world ? _world->startNode : 0), _trip(Trip::stay(_node, 0))
{
}

Trip Courier::trip() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _trip;
}

void Courier::publish(const Trip& trip)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _trip = trip;
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

void Courier::run()
{
    while (std::optional<Order> order = _inbox.take()) {
        // Cada tramo es una espera interrumpible: si se pide parar, el pedido queda pendiente.
        // Sin camino hasta el restaurante (moto atrapada, solo con mapas de evaluadores), no
        // puede tomar el pedido: queda pendiente y la moto sigue con el siguiente.
        const Leg toRestaurant = travel(order->restaurant(), true);
        if (toRestaurant == Leg::stopped) {
            return;
        }
        if (toRestaurant == Leg::noPath) {
            continue;
        }
        // En el restaurante: fila (ticket) hasta tomar un puesto. nullopt = se detuvo mientras esperaba.
        // El Slot libera el puesto al destruirse (RAII): en cualquier return de aquí en adelante.
        // (Slot es move-only sin asignación: el optional se construye directamente con el resultado.)
        std::optional<Restaurant::Slot> slot =
            _restaurants ? _restaurants->at(order->restaurant())->enter() : std::optional<Restaurant::Slot>();
        if (_restaurants && !slot) {
            return;
        }
        // Un evento negado (se pidió parar, o su t ya pasó el fin) también corta: el pedido queda pendiente.
        const std::int64_t startedAt = _clock.nowMs();
        if (!logEvent("pickupStarted", *order, startedAt, true)) {
            return;
        }
        if (_restaurants) {
            // Con el puesto tomado: espera la comida si no está lista, y la entrega dura kHandoverMs.
            const std::int64_t handedAt = std::max(startedAt, order->readyAtMs()) + kHandoverMs;
            if (_shutdown.sleepUntil(_clock.realTimeAt(handedAt))) {
                return;
            }
        }
        if (!logEvent("orderPickedUp", *order, _clock.nowMs(), true)) {
            return;
        }
        slot.reset();  // libera el puesto: entra el ticket siguiente

        // Al cliente hay camino desde el restaurante (si no, el pedido habría salido unreachable).
        const Leg toCustomer = travel(order->destination(), false);
        if (toCustomer == Leg::stopped) {
            return;
        }
        if (toCustomer == Leg::noPath) {
            continue;  // no debería pasar; si pasa, el pedido queda pendiente y la moto sigue
        }
        // El mismo t va al log y a las estadísticas: el resumen debe coincidir con el log.
        const std::int64_t deliveredAt = _clock.nowMs();
        if (!logEvent("orderDelivered", *order, deliveredAt, false)) {
            return;
        }

        const std::int64_t took = deliveredAt - order->createdAtMs();
        ++_delivered;
        _totalDeliveryMs += took;
        _maxDeliveryMs = std::max(_maxDeliveryMs, took);
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
