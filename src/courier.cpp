#include "courier.hpp"

#include <algorithm>
#include <optional>
#include <utility>

Courier::Courier(std::string id, const SimClock& clock, EventLog& log, std::int64_t legMs)
    : _id(std::move(id)), _clock(clock), _log(log), _legMs(legMs)
{
}

void Courier::run()
{
    while (std::optional<Order> order = _inbox.take()) {
        _clock.sleepSim(_legMs);  // viaje al restaurante
        const std::int64_t pickedAt = _clock.nowMs();
        logEvent("pickupStarted", *order, pickedAt, true);
        logEvent("orderPickedUp", *order, pickedAt, true);

        _clock.sleepSim(_legMs);  // viaje al cliente
        // El mismo t va al log y a las estadísticas: el resumen debe coincidir con el log.
        const std::int64_t deliveredAt = _clock.nowMs();
        logEvent("orderDelivered", *order, deliveredAt, false);

        const std::int64_t took = deliveredAt - order->createdAtMs();
        ++_delivered;
        _totalDeliveryMs += took;
        _maxDeliveryMs = std::max(_maxDeliveryMs, took);
    }
}

void Courier::logEvent(const char* event, const Order& order, std::int64_t t, bool withRestaurant)
{
    Event e;
    e.t = t;
    e.event = event;
    e.order = order.id();
    e.courier = _id;
    if (withRestaurant) {
        e.restaurant = order.restaurant();
    }
    _log.write(e);
}
