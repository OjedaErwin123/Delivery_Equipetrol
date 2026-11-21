// Prueba de FrameMeter (EC2, --frames): con instantes puestos a mano, cuenta los frames y da la mediana, el p99, el
// máximo, los lentos y los frames por segundo. Sale con 0 si todo está bien.
#include <chrono>
#include <cmath>
#include <iostream>

#include "frame_meter.hpp"
#include "log_line.hpp"

int main()
{
    bool ok = true;
    FrameMeter empty;
    ok &= check("sin frames: todo en 0", empty.frames() == 0 && empty.percentileMs(0.5) == 0 && empty.maxMs() == 0 &&
                                             empty.framesPerSecond() == 0);

    // 5 frames: intervalos de 30, 40, 35 y 100 ms (un frame lento).
    FrameMeter m;
    const auto t0 = FrameMeter::Clock::time_point{};
    using ms = std::chrono::milliseconds;
    for (int at : {0, 30, 70, 105, 205}) {
        m.tickAt(t0 + ms(at));
    }
    ok &= check("5 frames, 4 intervalos", m.frames() == 5);
    ok &= check("mediana = 35 ms (el 2.º de 30, 35, 40, 100)", std::fabs(m.percentileMs(0.5) - 35) < 1e-9);
    ok &= check("p99 y máximo = 100 ms", std::fabs(m.percentileMs(0.99) - 100) < 1e-9 && std::fabs(m.maxMs() - 100) < 1e-9);
    ok &= check("uno de más de 50 ms", m.slowerThan(50) == 1);
    ok &= check("4 intervalos en 205 ms = 19,5 por segundo", std::fabs(m.framesPerSecond() - 4000.0 / 205) < 1e-9);
    std::cout << m.summary() << "\n";
    return ok ? 0 : 1;
}
