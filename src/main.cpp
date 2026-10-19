// delivery_sim <config.json> [--log <path>]
//
// Lee y valida la configuración; si algo no se puede usar (argumentos, archivo, JSON,
// propiedades, referencias a nodos, rangos), lo explica en stderr y sale con código distinto
// de cero sin simular. Si no, abre la ventana y corre la simulación hasta durationS, hasta
// SIGINT/SIGTERM o hasta que se cierre la ventana. Sin pantalla, avisa y simula igual.
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

    const SimParams params = paramsFromConfig(config);
    // La ventana vive en este hilo (el principal), el mismo del bucle de frames que la dibuja.
    std::string whyNot;
    const std::unique_ptr<MapWindow> window = MapWindow::open(config, params.world, whyNot);
    FrameHook onFrame;
    if (window) {
        onFrame = [&window](const Frame& frame) { return window->draw(frame); };  // false: se cerró
    } else {
        std::cerr << "delivery_sim: sin ventana: " << whyNot << "; la simulación sigue igual\n";
    }

    installStopSignalHandlers();  // antes de lanzar hilos: Ctrl+C o kill detienen la simulación ordenadamente
    const Summary summary = runSimulation(params, log, std::cout, stopSignalFlag(), onFrame);
    std::cout << toJson(summary) << "\n";  // última línea de stdout
    return 0;
}
