#include "restaurant.hpp"

#include <utility>

Restaurant::Restaurant(std::string id, std::size_t pickupSlots) : _id(std::move(id)), _slots(pickupSlots) {}

std::optional<Restaurant::Slot> Restaurant::enter()
{
    std::unique_lock<std::mutex> lock(_mutex);
    const std::uint64_t ticket = _nextTicket++;  // el orden de llegada queda fijado aquí, con el lock tomado
    // El predicado cubre los despertares espurios y a los que despiertan sin ser su turno:
    // vuelven a dormir. Nadie gira comprobando: wait suelta el lock y duerme.
    _turn.wait(lock, [this, ticket] { return _aborted || (ticket == _nowServing && _inUse < _slots); });
    if (_aborted) {
        return std::nullopt;
    }
    ++_nowServing;
    ++_inUse;
    lock.unlock();
    // El ticket siguiente puede entrar ya si queda otro puesto libre (pickupSlots > 1).
    // notify_all y no notify_one: hay que despertar a UNA moto concreta (la del ticket
    // siguiente) y no se sabe cuál de las que duermen es; las demás vuelven a dormir.
    _turn.notify_all();
    return Slot(*this, ticket);
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
