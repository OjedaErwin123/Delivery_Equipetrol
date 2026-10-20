// delivery_sim <config.json> [--log <path>]
//
// Lee y valida la configuración; si algo no se puede usar (argumentos, archivo, JSON,
// propiedades, referencias a nodos, rangos), lo explica en stderr y sale con código distinto
// de cero sin simular. Si no, abre la ventana y corre la simulación hasta durationS, hasta
// SIGINT/SIGTERM o hasta que se cierre la ventana. Sin pantalla, avisa y simula igual.
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cli.hpp"
#include "config.hpp"
#include "simulation.hpp"
#include "stop_signal.hpp"
#include "window.hpp"

int main(int argc, char** argv)
{
    CliArgs args;
    Config config;
    try {
        args = parseArgs(std::vector<std::string>(argv + 1, argv + argc));
        config = loadConfig(args.configPath);
    } catch (const CliError& e) {
        std::cerr << "delivery_sim: " << e.what() << "\n" << usage();
        return 2;  // mal usado
    } catch (const ConfigError& e) {
        std::cerr << "delivery_sim: configuración inválida: " << e.what() << "\n";
        return 1;
    }

    std::ofstream log(args.logPath);
    if (!log) {
        std::cerr << "delivery_sim: no se pudo abrir '" << args.logPath << "' para escribir el log\n";
        return 1;
    }

    // La ventana vive en este hilo (el principal), el mismo del bucle de frames que la dibuja. Se abre
    // ANTES de crear cualquier hilo (paramsFromConfig ya lanza los Dijkstra con std::async): así sus
    // variables de entorno se ponen sin otros hilos, y la memoria de los hilos que crea Qt no aparece
    // en Valgrind con la pila de un hilo nuestro (glibc reutiliza las pilas de hilos terminados).
    std::string whyNot;
    const std::unique_ptr<MapWindow> window = MapWindow::open(config, whyNot);
    const SimParams params = paramsFromConfig(config);
    if (window) {
        window->setWorld(params.world);
    }
    FrameHook onFrame;
    // Cuándo se dibujó el último frame: ahí se vio la orden de parar (señal, tiempo o ventana). Lo
    // escribe y lo lee solo este hilo (el hook corre en el bucle de frames, en el hilo principal).
    auto lastFrame = std::chrono::steady_clock::now();
    bool closedFromWindow = false;
    if (window) {
        onFrame = [&window, &lastFrame, &closedFromWindow](const Frame& frame) {
            lastFrame = std::chrono::steady_clock::now();
            closedFromWindow = !window->draw(frame);  // se pidió terminar desde la ventana
            return !closedFromWindow;
        };
    } else {
        std::cerr << "delivery_sim: sin ventana: " << whyNot << "; la simulación sigue igual\n";
    }

    installStopSignalHandlers();  // antes de lanzar hilos: Ctrl+C o kill detienen la simulación ordenadamente
    const Summary summary = runSimulation(params, log, std::cout, stopSignalFlag(), onFrame);
    std::cout << toJson(summary) << std::endl;  // última línea de stdout (endl: sale ya, antes de la pantalla final)
    if (window) {
        // El resumen en la ventana. Tras una señal o un pedido desde la ventana, hasta 2 s después
        // del último frame (el que vio la orden): con lo que tardan en salir Qt y las herramientas
        // (medido: ~3,7 s en total bajo Valgrind) queda dentro de los 5 s reales del enunciado. Al
        // terminar por tiempo no hay ese límite: 5 s. Una tecla o la X la cierran antes.
        const bool askedToStop = stopSignalFlag().load() || closedFromWindow;
        window->showFinal(summary, lastFrame + std::chrono::seconds(askedToStop ? 2 : 5));
    }
    return 0;
}
