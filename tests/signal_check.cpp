// Prueba de la detención por señal: la simulación corre sin límite de tiempo y otro hilo le
// manda SIGINT o SIGTERM (dos veces seguidas: la segunda se ignora) en un momento al azar.
// Uso: signal_check [INT|TERM]   (sin argumento elige una al azar). Sale con 0 si todo está bien.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "log_line.hpp"
#include "simulation.hpp"
#include "stop_signal.hpp"

int main(int argc, char** argv)
{
    // El momento y la señal cambian en cada corrida (no hace falta repetirlos): se imprimen.
    std::mt19937 rng(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string name = argc > 1 ? argv[1] : (rng() % 2 == 0 ? "INT" : "TERM");
    const int signal = name == "TERM" ? SIGTERM : SIGINT;
    const auto delay = std::chrono::milliseconds(20 + rng() % 281);  // entre 20 y 300 ms reales

    SimParams p;
    p.couriers = 3;
    p.orders = 100000;      // nunca se llega: solo la señal la detiene
    p.meanIntervalMs = 1000;
    p.legMs = 5000;
    p.timeScale = 100.0;    // 1 s simulado = 10 ms real
    p.durationS = 0;        // sin límite de tiempo

    installStopSignalHandlers();
    std::chrono::steady_clock::time_point sentAt;  // lo escribe `sender`; se lee después de su join
    std::thread sender([&sentAt, delay, signal] {
        std::this_thread::sleep_for(delay);  // representa "un momento al azar", no coordina hilos
        sentAt = std::chrono::steady_clock::now();
        std::raise(signal);
        std::raise(signal);
    });
    std::ostringstream logOut;
    std::ostringstream out;
    const Summary s = runSimulation(p, logOut, out, stopSignalFlag());
    const auto returnedAt = std::chrono::steady_clock::now();
    sender.join();
    const auto afterSignalMs = std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - sentAt).count();

    int created = 0;
    int delivered = 0;
    int stopping = 0;
    std::int64_t stopT = -1;
    std::int64_t maxT = 0;
    bool noneAfterStopping = true;
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const std::string event = field(line, "event");
        maxT = std::max(maxT, timeOf(line));
        if (event == "simulationStopping") {
            ++stopping;
            stopT = timeOf(line);
            continue;
        }
        noneAfterStopping = noneAfterStopping && stopping == 0;
        created += event == "orderCreated" ? 1 : 0;
        delivered += event == "orderDelivered" ? 1 : 0;
    }

    std::cout << "SIG" << name << " a los " << delay.count() << " ms reales; simulationStopping t=" << stopT
              << "; creados " << created << ", entregados " << delivered << ", pendientes " << created - delivered
              << "; volvió " << afterSignalMs << " ms después de la señal\n";
    bool ok = true;
    ok &= check("línea de inicio", out.str() == "Simulation has started...\n");
    ok &= check("un solo simulationStopping (la segunda señal se ignora)", stopping == 1);
    ok &= check("simulationStopping es el último evento y el de mayor t", noneAfterStopping && stopT == maxT);
    ok &= check("resumen igual al log", s.created == created && s.delivered == delivered);
    // El enunciado da 5 s; aquí todo despierta enseguida (un frame de 33 ms + el join).
    ok &= check("vuelve poco después de la señal", afterSignalMs < 500);
    return ok ? 0 : 1;
}
