// delivery_sim <config.json> [--log <path>] [--headless] [--bench <csv>] [--bench-once] [--bench-app <nombre>]
//
// Con --bench o --bench-once no simula: mide las aplicaciones de GPU de EC2 (bench.hpp) y termina. Con
// --headless no abre la ventana (la simulación es la misma).
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

#include "bench.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "report.hpp"
#ifdef DELIVERY_USE_CUDA
#include "apsp.hpp"
#include "gpu/gpu_owner.hpp"
#endif
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

    if (args.bench()) {
        // EC2: no simula. Mide las aplicaciones de GPU con los datos de la configuración y termina.
        const SimParams params = paramsFromConfig(config);
        std::ofstream file;
        std::ostream discard(nullptr);  // --bench-once sin --bench: sin CSV
        if (!args.benchPath.empty()) {
            file.open(args.benchPath);
            if (!file) {
                std::cerr << "delivery_sim: no se pudo abrir '" << args.benchPath << "' para escribir el CSV\n";
                return 1;
            }
        }
        std::ostream& csv = args.benchPath.empty() ? discard : file;
        try {
            return runBenchmarks(params, csv, std::cerr, args.benchOnce ? 1 : kBenchRepetitions, args.benchApp);
        } catch (const std::exception& e) {  // p. ej. la GPU no se pudo preparar
            std::cerr << "delivery_sim: el bench falló: " << e.what() << "\n";
            return 1;
        }
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
    std::string whyNot = "--headless";
    const std::unique_ptr<MapWindow> window = args.headless ? nullptr : MapWindow::open(config, whyNot);
    SimParams params = paramsFromConfig(config);
    if (window) {
        window->setWorld(params.world);
    }
#ifdef DELIVERY_USE_CUDA
    // EC2: el hilo dueño de la GPU, para el prefiltro de las motos más cercanas (R1). Sin GPU usable, el
    // despachador sigue como siempre, en la CPU.
    try {
        params.gpu = std::make_shared<GpuOwner>(static_cast<std::size_t>(config.couriers));
        std::cerr << "delivery_sim: GPU: " << params.gpu->deviceName() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "delivery_sim: sin GPU (" << e.what() << "); el despacho sigue en la CPU\n";
    }
    // EC2, T1: la alcanzabilidad de cada destino desde cada restaurante (unreachable), desde la tabla de todos
    // contra todos que calcula la GPU (Floyd–Warshall por bloques). Es la misma que la de los Dijkstra
    // (apsp_check); si la GPU falla, queda esa.
    if (params.gpu) {
        try {
            const RoadGraph& graph = params.world->graph;
            const auto start = std::chrono::steady_clock::now();
            const GpuOwner::Apsp solved = params.gpu->apsp(initialTable(graph)).get().take();
            std::vector<std::size_t> restaurantNodes, destinationNodes;
            for (const std::string& r : params.restaurants) {
                restaurantNodes.push_back(params.world->restaurantNode.at(r));
            }
            for (const std::string& d : params.destinations) {
                destinationNodes.push_back(*graph.nodeIndex(d));
            }
            params.reachable = reachabilityFrom(solved.table, restaurantNodes, destinationNodes);
            std::cerr << "delivery_sim: tabla de todos contra todos (" << graph.nodeCount() << " nodos) en la GPU: "
                      << solved.timing.kernelMs << " ms de kernels, "
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                      << " ms en total\n";
        } catch (const std::exception& e) {
            std::cerr << "delivery_sim: la tabla de todos contra todos falló en la GPU (" << e.what()
                      << "); la alcanzabilidad queda la de los Dijkstra\n";
        }
    }
#endif
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
    if (!args.reportPath.empty()) {
        // EC2: el reporte de las aplicaciones de tipo A. Recién acá, con la simulación terminada y el resumen ya
        // escrito: relee el log (una "repetición del log") y no cambia nada de lo que pasó.
        log.flush();
        std::ifstream replayed(args.logPath);
        std::ofstream report(args.reportPath);
        if (!replayed || !report) {
            std::cerr << "delivery_sim: no se pudo releer el log o escribir '" << args.reportPath << "'\n";
            return 1;
        }
        try {
            writeReport(replayLog(replayed), params.gpu.get(), report);
            std::cerr << "delivery_sim: reporte en '" << args.reportPath << "'\n";
        } catch (const std::exception& e) {
            std::cerr << "delivery_sim: el reporte falló: " << e.what() << "\n";
            return 1;
        }
    }
    return 0;
}
