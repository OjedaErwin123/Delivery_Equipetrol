#include "restaurant.hpp"

#include <utility>

Restaurant::Restaurant(std::string id, std::size_t pickupSlots) : _id(std::move(id)), _slots(pickupSlots) {}

std::optional<Restaurant::Slot> Restaurant::enter(const SimClock& clock)
{
    std::unique_lock<std::mutex> lock(_mutex);
    const std::uint64_t ticket = _nextTicket++;  // el orden de llegada queda fijado aquí, con el lock tomado
    // La hora de llegada se lee con el MISMO lock que el ticket. Leída antes, la moto A podría leer
    // 100, la B 101 y ganarle B el lock: el log diría que A llegó antes y fue atendida después,
    // aunque los tickets se respetaron. Hora y ticket tienen que salir juntos (una foto consistente).
    const std::int64_t arrivedAt = clock.nowMs();
    // El predicado cubre los despertares espurios y a los que despiertan sin ser su turno:
    // vuelven a dormir. Nadie gira comprobando: wait suelta el lock y duerme.
    _turn.wait(lock, [this, ticket] { return _aborted || (ticket == _nowServing && _inUse < _slots); });
    if (_aborted) {
        return std::nullopt;
    }
    ++_nowServing;
    ++_inUse;
    const std::int64_t startedAt = clock.nowMs();  // también con el lock: en orden de ticket
    lock.unlock();
    // El ticket siguiente puede entrar ya si queda otro puesto libre (pickupSlots > 1).
    // notify_all y no notify_one: hay que despertar a UNA moto concreta (la del ticket
    // siguiente) y no se sabe cuál de las que duermen es; las demás vuelven a dormir.
    _turn.notify_all();
    return Slot(*this, ticket, arrivedAt, startedAt);
}

void Restaurant::leave()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        --_inUse;
    }
    _turn.notify_all();  // mismo motivo que en enter(): el que sigue es una moto concreta
}

void Restaurant::abort()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _aborted = true;
    }
    _turn.notify_all();
}

std::size_t Restaurant::inUse() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _inUse;
}
