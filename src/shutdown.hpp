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
class Shutdown {
public:
    // Pide parar y despierta a todos los que duermen. Llamarla más de una vez no hace nada nuevo.
    void request()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _requested = true;
        }
        _cv.notify_all();
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
