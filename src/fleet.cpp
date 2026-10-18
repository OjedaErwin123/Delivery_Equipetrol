#include "fleet.hpp"

Fleet::Fleet(std::size_t couriers, std::size_t bagCapacity) : _capacity(bagCapacity), _assigned(couriers, 0) {}

std::optional<std::size_t> Fleet::reserve(std::size_t from)
{
    std::unique_lock<std::mutex> lock(_mutex);
    const std::size_t n = _assigned.size();
    std::optional<std::size_t> chosen;
    // El predicado elige la moto: cubre los despertares espurios (si sigue todo lleno, vuelve a dormir).
    _space.wait(lock, [&] {
        for (std::size_t k = 0; k < n && !_aborted; ++k) {
            const std::size_t i = (from + k) % n;
            if (_assigned[i] < _capacity) {
                chosen = i;
                return true;
            }
        }
        return _aborted;
    });
    if (_aborted) {
        return std::nullopt;
    }
    ++_assigned[*chosen];  // con el mismo lock con que se eligió: nadie puede llenarla entre medio
    return chosen;
}

void Fleet::release(std::size_t courier)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        --_assigned[courier];
    }
    // Solo el despachador espera en reserve: alcanza con despertar a uno.
    _space.notify_one();
}

void Fleet::abort()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _aborted = true;
    }
    _space.notify_all();
}

std::size_t Fleet::assigned(std::size_t courier) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _assigned[courier];
}
