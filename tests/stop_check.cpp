// Prueba de la detención por tiempo: al cumplirse durationS la simulación deja de crear
// pedidos, registra simulationStopping, termina todos los hilos enseguida (aunque haya
// motos a mitad de un tramo) y lo no entregado queda pendiente. Sale con 0 si todo está bien.
//
// Se corre con varias duraciones: el bucle de frames despierta cada 33 ms reales, y un error
// en la detención solo se nota si el fin cae lejos de un frame. Con una sola duración, el fin
// puede caer justo en un frame y la prueba pasar por casualidad (pasó con 20 s: el código de
// antes del PR #16 pasaba; recorriendo 20..29 s falla 36 de 50 corridas).
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

constexpr double kTimeScale = 100.0;  // 1 s simulado = 10 ms real

// Lo que se verifica en cada corrida; cada campo es true si se cumplió.
struct Result {
    bool oneStopping = false;
    bool createdBeforeEnd = true;   // ningún orderCreated desde el fin; simulationStopping con t = fin
    bool noneAfterStopping = true;  // ningún evento después de simulationStopping, en el orden del archivo
    bool allBeforeEnd = true;       // y ninguno con t >= durationS*1000: la simulación ya terminó
    bool summaryMatches = false;
    bool pendingLeft = false;
    bool stopsQuickly = false;
};

Result runOnce(std::int64_t durationS)
{
    SimParams p;
    p.couriers = 2;
    p.orders = 1000;        // muchos más de los que caben en durationS: el fin es por tiempo
    p.meanIntervalMs = 1000;
    p.legMs = 5000;         // 50 ms reales por tramo: al detener hay motos viajando
    p.timeScale = kTimeScale;
    p.durationS = durationS;
    const std::int64_t endMs = durationS * 1000;

    std::ostringstream logOut;
    std::ostringstream out;
    const auto start = std::chrono::steady_clock::now();
    const Summary s = runSimulation(p, logOut, out);
    const auto realMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    Result r;
    int created = 0;
    int delivered = 0;
    int stopping = 0;
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const std::string event = field(line, "event");
        if (event != "simulationStopping") {
            r.noneAfterStopping = r.noneAfterStopping && stopping == 0;
            r.allBeforeEnd = r.allBeforeEnd && timeOf(line) < endMs;
        }
        if (event == "orderCreated") {
            ++created;
            r.createdBeforeEnd = r.createdBeforeEnd && timeOf(line) < endMs;
        } else if (event == "orderDelivered") {
            ++delivered;
        } else if (event == "simulationStopping") {
            ++stopping;
            r.createdBeforeEnd = r.createdBeforeEnd && timeOf(line) == endMs;
        }
    }
    r.oneStopping = stopping == 1;
    r.summaryMatches = s.created == created && s.delivered == delivered;
    r.pendingLeft = created - delivered > 0;
    // La duración real + margen: los hilos no esperan a terminar su tramo.
    const auto durationRealMs = static_cast<std::int64_t>(static_cast<double>(endMs) / kTimeScale);
    r.stopsQuickly = realMs < durationRealMs + 300;

    std::cout << "  durationS " << durationS << ": real " << realMs << " ms; creados " << created << ", entregados "
              << delivered << ", pendientes " << created - delivered << "\n";
    return r;
}

}  // namespace

int main()
{
    Result all;
    all.oneStopping = all.summaryMatches = all.pendingLeft = all.stopsQuickly = true;
    for (std::int64_t durationS = 20; durationS <= 29; ++durationS) {
        const Result r = runOnce(durationS);
        all.oneStopping = all.oneStopping && r.oneStopping;
        all.createdBeforeEnd = all.createdBeforeEnd && r.createdBeforeEnd;
        all.noneAfterStopping = all.noneAfterStopping && r.noneAfterStopping;
        all.allBeforeEnd = all.allBeforeEnd && r.allBeforeEnd;
        all.summaryMatches = all.summaryMatches && r.summaryMatches;
        all.pendingLeft = all.pendingLeft && r.pendingLeft;
        all.stopsQuickly = all.stopsQuickly && r.stopsQuickly;
    }

    bool ok = true;
    ok &= check("un solo simulationStopping", all.oneStopping);
    ok &= check("ningún pedido creado desde el fin; simulationStopping con t = durationS*1000", all.createdBeforeEnd);
    ok &= check("simulationStopping es el último evento del log", all.noneAfterStopping);
    ok &= check("ningún evento con t >= durationS*1000", all.allBeforeEnd);
    ok &= check("resumen igual al log", all.summaryMatches);
    ok &= check("quedan pendientes (había motos a mitad de tramo)", all.pendingLeft);
    ok &= check("termina poco después del fin", all.stopsQuickly);
    return ok ? 0 : 1;
}
