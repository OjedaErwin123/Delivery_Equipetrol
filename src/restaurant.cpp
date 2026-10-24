#include "restaurant.hpp"

#include <utility>

Restaurant::Restaurant(std::string id, std::size_t pickupSlots, PauseSchedule pauses)
    : _id(std::move(id)), _slots(pickupSlots), _pauses(pauses)
{
}

std::optional<Restaurant::Slot> Restaurant::enter(const SimClock& clock)
{
    std::unique_lock<std::mutex> lock(_mutex);
    const std::uint64_t ticket = _nextTicket++;  // el orden de llegada queda fijado aquí, con el lock tomado
    // La hora de llegada se lee con el MISMO lock que el ticket. Leída antes, la moto A podría leer
    // 100, la B 101 y ganarle B el lock: el log diría que A llegó antes y fue atendida después,
    // aunque los tickets se respetaron. Hora y ticket tienen que salir juntos (una foto consistente).
    const std::int64_t arrivedAt = clock.nowMs();
    // Cada vuelta vuelve a evaluar la condición, así cubre los despertares espurios y a los que
    // despiertan sin ser su turno. Nadie gira comprobando: en cada vuelta duerme.
    for (;;) {
        if (_aborted) {
            return std::nullopt;
        }
        // La pausa se pregunta con el lock tomado y con el mismo reloj que startedAt: ningún
        // startedAt puede caer dentro de una pausa.
        const std::int64_t now = clock.nowMs();
        const bool paused = _pauses.pausedAt(now);
        if (!paused && ticket == _nowServing && _inUse < _slots) {
            break;
        }
        if (paused) {
            // El fin de la pausa no lo avisa nadie (lo trae el paso del tiempo): espera CON plazo.
            // +1 ms simulado porque nowMs() trunca: al despertar justo en el fin podría dar fin - 1,
            // seguir "en pausa" y volver enseguida con el plazo ya vencido.
            _turn.wait_until(lock, clock.realTimeAt(_pauses.pauseEnd(now) + 1));
        } else {
            _turn.wait(lock);  // lo cambia otra moto: la despierta leave() o la que entra
        }
    }
    ++_nowServing;
    ++_inUse;
    const std::int64_t startedAt = clock.nowMs();  // también con el lock: en orden de ticket
    // Las demoras por pausa de esta visita, una sola vez y con tiempos simulados (sin doble conteo):
    // cada pausa que empezó mientras esperaba, y una más si llegó en plena pausa.
    _delayedByPause += _pauses.startsBetween(arrivedAt, startedAt) + (_pauses.pausedAt(arrivedAt) ? 1 : 0);
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

std::int64_t Restaurant::delayedByPause() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _delayedByPause;
}

std::size_t Restaurant::inUse() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _inUse;
}
