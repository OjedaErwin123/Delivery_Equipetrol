#include "fleet.hpp"

Fleet::Fleet(std::size_t couriers, std::size_t bagCapacity)
    : _capacity(bagCapacity), _assigned(couriers, 0), _broken(couriers, false), _retired(couriers, false)
{
}

std::optional<std::size_t> Fleet::reserve(std::size_t from)
{
    std::unique_lock<std::mutex> lock(_mutex);
    const std::size_t n = _assigned.size();
    std::optional<std::size_t> chosen;
    // El predicado elige la moto: cubre los despertares espurios (si sigue todo lleno, vuelve a dormir).
    _space.wait(lock, [&] {
        for (std::size_t k = 0; k < n && !_aborted; ++k) {
            const std::size_t i = (from + k) % n;
            if (hasSpace(i)) {
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

std::vector<std::size_t> Fleet::withSpace() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::size_t> free;
    for (std::size_t i = 0; i < _assigned.size(); ++i) {
        if (hasSpace(i)) {
            free.push_back(i);
        }
    }
    return free;
}

bool Fleet::tryReserve(std::size_t courier)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_aborted || !hasSpace(courier)) {
        return false;
    }
    ++_assigned[courier];
    return true;
}

std::uint64_t Fleet::version() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _version;
}

bool Fleet::waitForChange(std::uint64_t seen, std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock<std::mutex> lock(_mutex);
    _space.wait_until(lock, deadline, [this, seen] { return _aborted || _version != seen; });
    return !_aborted;
}

bool Fleet::waitForChange(std::uint64_t seen)
{
    std::unique_lock<std::mutex> lock(_mutex);
    _space.wait(lock, [this, seen] { return _aborted || _version != seen; });
    return !_aborted;
}

void Fleet::beginTransfer()
{
    std::lock_guard<std::mutex> lock(_mutex);
    ++_transfers;
}

void Fleet::endTransfer()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        --_transfers;
        ++_version;
    }
    _space.notify_all();  // el despachador vuelve a poder reservar
}

std::vector<std::size_t> Fleet::canReceive(std::size_t count, std::size_t except) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < _assigned.size(); ++i) {
        if (i != except && !_broken[i] && !_retired[i] && _assigned[i] + count <= _capacity) {
            out.push_back(i);
        }
    }
    return out;
}

void Fleet::retire(std::size_t courier)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _retired[courier] = true;
        ++_version;
    }
    _space.notify_all();
}

bool Fleet::othersAlive(std::size_t except) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (std::size_t i = 0; i < _retired.size(); ++i) {
        if (i != except && !_retired[i]) {
            return true;
        }
    }
    return false;
}

void Fleet::release(std::size_t courier)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        --_assigned[courier];
        ++_version;
    }
    // Esperan el despachador y las motos averiadas que buscan receptor: se despierta a todos.
    _space.notify_all();
}

bool Fleet::moveAssigned(std::size_t from, std::size_t to, std::size_t count, const std::function<bool()>& commit)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_aborted || _broken[to] || _retired[to] || _assigned[to] + count > _capacity || !commit()) {
            return false;
        }
        _assigned[from] -= count;
        _assigned[to] += count;
        ++_version;
    }
    _space.notify_all();
    return true;
}

void Fleet::setBroken(std::size_t courier, bool broken)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _broken[courier] = broken;
        ++_version;
    }
    _space.notify_all();
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
