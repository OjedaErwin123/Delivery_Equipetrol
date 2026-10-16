#pragma once

#include <chrono>
#include <cstdint>

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

    // Instante real en que el reloj marcará `simMs` (milisegundos simulados desde el inicio).
    std::chrono::steady_clock::time_point realTimeAt(std::int64_t simMs) const
    {
        return _start + toReal(simMs);
    }

    // Instante real dentro de `simMs` milisegundos simulados a partir de ahora.
    std::chrono::steady_clock::time_point realTimeIn(std::int64_t simMs) const
    {
        return std::chrono::steady_clock::now() + toReal(simMs);
    }

private:
    // Duración real de `simMs` milisegundos simulados. Las esperas se hacen con estos instantes
    // en Shutdown::sleepUntil, que se puede interrumpir (el reloj ya no duerme por sí mismo).
    std::chrono::steady_clock::duration toReal(std::int64_t simMs) const
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(simMs / _timeScale));
    }

    const double _timeScale;  // segundos simulados por segundo real
    const std::chrono::steady_clock::time_point _start;
};
