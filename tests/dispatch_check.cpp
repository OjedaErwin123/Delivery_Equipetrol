// Prueba del despachador real (estimaciones en el pool): con motos disponibles todo se asigna y
// se entrega; con acceptTimeoutMs = 0 todo sale noCourier; y si las estimaciones FALLAN (motos sin
// camino), el error llega al despachador, se reporta en stderr, el programa sigue y los pedidos
// salen noCourier al vencer. Sale con 0 si todo está bien.
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "log_line.hpp"
#include "simulation.hpp"

namespace {

// a→b→c de sentido único, a–d–c de doble sentido, e sin calles; r0 en b y r1 en d (1 m por ms).
Config smallTown(const std::string& start, std::int64_t acceptTimeoutMs, bool withIsland)
{
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}};
    if (withIsland) {
        c.nodes.push_back({"e", 0.002, 0.002});
    }
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "d", 2, 0, 0}};
    c.couriers = 2;
    c.bagCapacity = 3;
    c.speedKmh = 3600;  // 1 m por ms simulado
    c.startNode = start;
    c.meanIntervalMs = 5000;
    c.burstMax = 2;
    c.maxPending = 50;
    c.seed = 3;
    c.quoteTimeoutMs = 200;
    c.acceptTimeoutMs = acceptTimeoutMs;
    c.durationS = 0;
    c.timeScale = 1000;  // 1 s simulado = 1 ms real
    return c;
}

struct Run {
    Summary summary;
    std::map<std::string, int> events;  // evento[:motivo] -> cuántos
    std::string err;
};

Run simulate(const Config& c, int orders)
{
    SimParams p = paramsFromConfig(c);
    p.orders = orders;
    std::ostringstream logOut, out, err;
    std::streambuf* original = std::cerr.rdbuf(err.rdbuf());  // solo el despachador escribe en stderr
    Run run;
    run.summary = runSimulation(p, logOut, out);  // vuelve después del join de todos los hilos
    std::cerr.rdbuf(original);
    run.err = err.str();
    std::istringstream in(logOut.str());
    for (std::string line; std::getline(in, line);) {
        const std::string reason = field(line, "reason");
        ++run.events[field(line, "event") + (reason.empty() ? "" : ":" + reason)];
    }
    return run;
}

}  // namespace

int main()
{
    bool ok = true;

    // 1. Normal: dos motos que salen de a, 30 pedidos: todos asignados por estimación y entregados.
    Run normal = simulate(smallTown("a", 600000, false), 30);
    std::cout << "normal: " << toJson(normal.summary) << "\n";
    ok &= check("normal: los 30 asignados y entregados, sin rechazos ni errores",
                normal.events["orderAssigned"] == 30 && normal.summary.delivered == 30 &&
                    normal.summary.rejected == 0 && normal.err.empty());

    // 2. acceptTimeoutMs = 0: ningún pedido llega a asignarse a tiempo: todos noCourier.
    Run expired = simulate(smallTown("a", 0, false), 20);
    ok &= check("acceptTimeoutMs = 0: los 20 salen noCourier, ninguno asignado",
                expired.events["orderRejected:noCourier"] == 20 && expired.events["orderAssigned"] == 0 &&
                    expired.summary.rejected == 20);

    // 3. Las motos salen de e (sin calles): toda estimación falla (sin camino). El error cruza de
    //    hilo por el future, se reporta en stderr y el programa sigue; al vencer (5 s simulados),
    //    noCourier. Los pedidos hacia e salen unreachable (nadie llega a e desde un restaurante).
    Run stuck = simulate(smallTown("e", 5000, true), 10);
    const int noCourier = stuck.events["orderRejected:noCourier"];
    const int unreachable = stuck.events["orderRejected:unreachable"];
    std::cout << "motos atrapadas: noCourier " << noCourier << ", unreachable " << unreachable
              << "; primera línea de stderr: " << stuck.err.substr(0, stuck.err.find('\n')) << "\n";
    ok &= check("estimaciones fallidas: reportadas en stderr (sin camino) y el programa sigue",
                stuck.err.find("estimación fallida") != std::string::npos &&
                    stuck.err.find("sin camino de e") != std::string::npos);
    ok &= check("estimaciones fallidas: nada asignado; cada pedido sale noCourier o unreachable",
                stuck.events["orderAssigned"] == 0 && noCourier > 0 && noCourier + unreachable == 10 &&
                    stuck.summary.rejected == 10);
    return ok ? 0 : 1;
}
