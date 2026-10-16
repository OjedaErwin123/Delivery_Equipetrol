#pragma once

#include <chrono>
#include <condition_variable>
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
class Shutdown {
public:
    // Pide parar, corre `action` en la misma sección crítica (p. ej. registrar
    // simulationStopping) y despierta a todos los que duermen.
    template <typename Action>
    void request(Action action)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _requested = true;
            action();
        }
        _cv.notify_all();
    }

    void request()
    {
        request([] {});
    }

    // Corre `action` solo si todavía no se pidió parar, con el lock tomado: lo que haga
    // queda antes de la acción de request(). Devuelve false si ya se había pedido parar.
    template <typename Action>
    bool unlessStopped(Action action) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_requested) {
            return false;
        }
        action();
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
    bool _requested = false;
};
