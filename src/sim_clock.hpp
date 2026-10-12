#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

// Reloj de la simulación.
//
// t_simulado = (ahora_real - inicio_real) * timeScale, con steady_clock (nunca
// retrocede aunque cambie la hora del sistema). t = 0 es el instante en que se
// crea el reloj, justo antes de imprimir "Simulation has started...".
//
// Lo leen todos los hilos a la vez, pero ninguno lo modifica: sus dos campos son
// const y se fijan al construirlo, así que no hay data race posible ni hace falta lock.
class SimClock {
public:
    explicit SimClock(double timeScale)
        : _timeScale(timeScale), _start(std::chrono::steady_clock::now())
    {
    }

    // Milisegundos simulados desde el inicio.
    std::int64_t nowMs() const
    {
        const std::chrono::duration<double, std::milli> real = std::chrono::steady_clock::now() - _start;
        return static_cast<std::int64_t>(real.count() * _timeScale);
    }

    // Deja pasar `simMs` milisegundos simulados (simMs / timeScale reales).
    // Pausar solo representa tiempo simulado; nunca se usa para esperar a otro hilo.
    void sleepSim(std::int64_t simMs) const
    {
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(simMs / _timeScale));
    }

private:
    const double _timeScale;  // segundos simulados por segundo real
    const std::chrono::steady_clock::time_point _start;
};
