// Prueba de la detención por tiempo: al cumplirse durationS la simulación deja de crear
// pedidos, registra simulationStopping, termina todos los hilos enseguida (aunque haya
// motos a mitad de un tramo) y lo no entregado queda pendiente. Sale con 0 si todo está bien.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include "log_line.hpp"
#include "simulation.hpp"

int main()
{
    SimParams p;
    p.couriers = 2;
    p.orders = 1000;        // muchos más de los que caben en durationS: el fin es por tiempo
    p.meanIntervalMs = 1000;
    p.legMs = 5000;         // 50 ms reales por tramo: al detener hay motos viajando
    p.timeScale = 100.0;    // 1 s simulado = 10 ms real
    p.durationS = 20;       // 200 ms reales
    const std::int64_t endMs = p.durationS * 1000;

    std::ostringstream logOut;
    std::ostringstream out;
    const auto start = std::chrono::steady_clock::now();
    const Summary s = runSimulation(p, logOut, out);
    const auto realMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    int created = 0;
    int delivered = 0;
    int stopping = 0;
    bool createdBeforeEnd = true;
    bool noneAfterStopping = true;  // ningún evento después de simulationStopping, en el orden del archivo
    bool allBeforeEnd = true;       // y ninguno con t >= durationS*1000: la simulación ya terminó
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const std::string event = field(line, "event");
        if (event != "simulationStopping") {
            noneAfterStopping = noneAfterStopping && stopping == 0;
            allBeforeEnd = allBeforeEnd && timeOf(line) < endMs;
        }
        if (event == "orderCreated") {
            ++created;
            createdBeforeEnd = createdBeforeEnd && timeOf(line) < endMs;
        } else if (event == "orderDelivered") {
            ++delivered;
        } else if (event == "simulationStopping") {
            ++stopping;
            createdBeforeEnd = createdBeforeEnd && timeOf(line) == endMs;
        }
    }

    std::cout << "real: " << realMs << " ms; creados " << created << ", entregados " << delivered
              << ", pendientes " << created - delivered << "\n";
    bool ok = true;
    ok &= check("un solo simulationStopping", stopping == 1);
    ok &= check("ningún pedido creado desde el fin; simulationStopping con t = durationS*1000", createdBeforeEnd);
    ok &= check("simulationStopping es el último evento del log", noneAfterStopping);
    ok &= check("ningún evento con t >= durationS*1000", allBeforeEnd);
    ok &= check("resumen igual al log", s.created == created && s.delivered == delivered);
    ok &= check("quedan pendientes (había motos a mitad de tramo)", created - delivered > 0);
    // 200 ms de simulación + margen: los hilos no esperan a terminar su tramo.
    ok &= check("termina poco después del fin", realMs < 200 + 300);
    return ok ? 0 : 1;
}
