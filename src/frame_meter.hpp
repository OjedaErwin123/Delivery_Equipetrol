#pragma once

// EC2 (--frames): cuánto dura cada vuelta del bucle de frames. Responde si la ventana mantiene sus ~30 frames por
// segundo con la GPU y sin ella (sección 2.2 del documento de EC2). Lo usa solo el hilo principal (el hook del bucle
// de frames): no se comparte, no necesita lock. No cambia la simulación: solo mide entre una llamada y la siguiente.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

class FrameMeter {
public:
    using Clock = std::chrono::steady_clock;

    // Una vuelta del bucle (se llama al principio del hook de cada frame).
    void tick() { tickAt(Clock::now()); }
    void tickAt(Clock::time_point now)
    {
        if (_started) {
            _gapsMs.push_back(std::chrono::duration<double, std::milli>(now - _last).count());
        }
        _last = now;
        _started = true;
    }

    std::size_t frames() const { return _started ? _gapsMs.size() + 1 : 0; }

    // El percentil q (0..1) de los intervalos, por el método del vecino más cercano: el valor de la posición
    // ceil(q * n) en los intervalos ordenados (q = 0,5 con 4 intervalos: el 2.º). 0 si no hay intervalos.
    double percentileMs(double q) const
    {
        if (_gapsMs.empty()) {
            return 0;
        }
        std::vector<double> sorted = _gapsMs;
        std::sort(sorted.begin(), sorted.end());
        const double rank = q * static_cast<double>(sorted.size());
        std::size_t index = static_cast<std::size_t>(rank);
        if (static_cast<double>(index) == rank && index > 0) {
            --index;  // rank exacto: la posición rank (contando desde 1)
        }
        return sorted[std::min(index, sorted.size() - 1)];
    }

    double maxMs() const { return _gapsMs.empty() ? 0 : *std::max_element(_gapsMs.begin(), _gapsMs.end()); }

    // Cuántos intervalos superan `limitMs` (un frame "perdido": con 33 ms por frame, uno de más de 50 ms se nota).
    std::size_t slowerThan(double limitMs) const
    {
        return static_cast<std::size_t>(
            std::count_if(_gapsMs.begin(), _gapsMs.end(), [limitMs](double g) { return g > limitMs; }));
    }

    // Frames por segundo en promedio: intervalos / tiempo entre el primer frame y el último.
    double framesPerSecond() const
    {
        double total = 0;
        for (double g : _gapsMs) {
            total += g;
        }
        return total > 0 ? 1000.0 * static_cast<double>(_gapsMs.size()) / total : 0;
    }

    // Una línea para stderr (la lee tools/efecto_simulacion.py).
    std::string summary() const
    {
        char line[256];
        std::snprintf(line, sizeof line,
                      "frames: %zu, por segundo %.2f, intervalo mediana %.2f ms, p99 %.2f ms, máximo %.2f ms, "
                      "de más de 50 ms: %zu",
                      frames(), framesPerSecond(), percentileMs(0.5), percentileMs(0.99), maxMs(), slowerThan(50));
        return line;
    }

private:
    std::vector<double> _gapsMs;
    Clock::time_point _last{};
    bool _started = false;
};
