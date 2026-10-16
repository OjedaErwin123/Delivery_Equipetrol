#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// La orden de parar la simulación, compartida por todos los hilos: un monitor.
//
// Las pausas que representan tiempo simulado duermen aquí con wait_until: el hilo se
// despierta a la hora prevista O apenas se pide parar, lo que ocurra primero. Así, tras
// la orden de parar, ningún hilo sigue dormido un viaje entero (hay solo 5 s reales).
// _requested solo se lee y escribe con _mutex tomado: no hay data race.
//
// Las acciones de request() y unlessStopped() corren con _mutex tomado, así "comprobar y
// actuar" es una sola operación (sin check-then-act). Pueden tomar otro lock (el del log),
// pero nunca uno que se tome antes que este: el orden es siempre Shutdown -> log.
//
// Todo evento del log se registra con unlessStopped: así simulationStopping (la acción de
// request) es siempre el último evento, y ninguno tiene t posterior al fin.
class Shutdown {
public:
    // `endMs`: instante simulado del fin por tiempo (0 = sin límite). Es const: no hay data race.
    explicit Shutdown(std::int64_t endMs = 0) : _endMs(endMs) {}

    // Pide parar, corre `action` en la misma sección crítica (p. ej. registrar
    // simulationStopping) y despierta a todos los que duermen. `action` recibe el mayor t
    // registrado hasta ahora: el evento final puede llevar un t que ninguno anterior supere.
    template <typename Action>
    void request(Action action)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _requested = true;
            action(_latestT);
        }
        _cv.notify_all();
    }

    // Corre `action` (registrar un evento con tiempo `t`) solo si todavía no se pidió parar
    // y `t` es anterior al fin, con el lock tomado: lo que haga queda antes de la acción de
    // request(). Devuelve false si no la corrió: quien llama debe abandonar lo que hacía.
    template <typename Action>
    bool unlessStopped(std::int64_t t, Action action) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_requested || (_endMs > 0 && t >= _endMs)) {
            return false;
        }
        action();
        _latestT = std::max(_latestT, t);
        return true;
    }

    bool requested() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _requested;
    }

    // Duerme hasta `deadline` sin consumir CPU. Devuelve true si se pidió parar (antes o
    // durante la espera): quien llama debe abandonar lo que estaba haciendo.
    bool sleepUntil(std::chrono::steady_clock::time_point deadline) const
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_until(lock, deadline, [this] { return _requested; });
    }

private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _cv;
    const std::int64_t _endMs;
    bool _requested = false;
    mutable std::int64_t _latestT = 0;  // mayor t registrado con unlessStopped; con _mutex tomado
};
