// Prueba de las versiones de CPU de T1 (EC2): la tabla de distancias de todos contra todos. Uso: apsp_check
// <config.json>. Sobre la red de la configuración:
//  - Floyd–Warshall por bloques con N hilos da la misma tabla que el clásico de 1 hilo (enteros: bit a bit), con 1, 2, 3 y 4 hilos;
//  - la alcanzabilidad desde cada restaurante a cada nodo es IDÉNTICA a la de los Dijkstra de RoadGraph (la que
//    usa la simulación para unreachable), también con una red con calles quitadas (nodos inalcanzables de verdad);
//  - la distancia entera difiere de la de Dijkstra (en double) a lo sumo en medio metro por calle del camino: es el
//    redondeo de cada calle al metro, la tolerancia de la tabla.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

#include "apsp.hpp"
#include "config.hpp"
#include "log_line.hpp"
#include "road_graph.hpp"

namespace {

bool sameReachability(const RoadGraph& graph, const Config& config, const DistanceTable& table)
{
    bool same = true;
    for (std::size_t r = 0; r < config.restaurants.size(); ++r) {
        const std::size_t from = *graph.nodeIndex(config.restaurants[r].node);
        for (std::size_t node = 0; node < graph.nodeCount(); ++node) {
            same &= graph.reachableFromRestaurant(r, node) == (table.at(from, node) != DistanceTable::kUnreachable);
        }
    }
    return same;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "uso: apsp_check <config.json>\n";
        return 1;
    }
    const Config config = loadConfig(argv[1]);
    const RoadGraph graph(config);
    const DistanceTable initial = initialTable(graph);
    DistanceTable reference = initial;
    floydWarshall(reference);
    bool ok = true;
    bool parallelSame = true;
    for (std::size_t threads : {1, 2, 3, 4}) {
        ThreadPool pool(threads);
        DistanceTable table = initial;
        floydWarshallBlocked(table, pool);
        parallelSame &= table.meters == reference.meters;
    }
    ok &= check("por bloques con 1, 2, 3 y 4 hilos = clásico con 1 hilo (bit a bit)", parallelSame);
    ok &= check("alcanzabilidad desde cada restaurante = la de los Dijkstra de RoadGraph",
                sameReachability(graph, config, reference));

    // Con calles quitadas (la mitad de las de doble sentido pasan a ser de sentido único y se quitan algunas):
    // aparecen nodos inalcanzables y la comparación tiene casos de los dos lados.
    Config cut = config;
    for (std::size_t s = 0; s < cut.streets.size(); ++s) {
        if (s % 2 == 0) {
            cut.streets[s].oneWay = true;
        }
    }
    cut.streets.resize(cut.streets.size() * 4 / 5);
    const RoadGraph cutGraph(cut);
    DistanceTable cutTable = initialTable(cutGraph);
    floydWarshall(cutTable);
    std::size_t unreachable = 0;
    for (std::size_t i = 0; i < cutTable.n * cutTable.stride; ++i) {
        unreachable += cutTable.meters[i] == DistanceTable::kUnreachable;
    }
    std::cout << "red recortada: " << unreachable << " pares sin camino (contando el relleno)\n";
    ok &= check("con la red recortada, la alcanzabilidad también es idéntica a la de los Dijkstra",
                sameReachability(cutGraph, cut, cutTable));

    // La tolerancia de las distancias: medio metro por calle del camino mínimo.
    double worst = 0;
    bool withinTolerance = true;
    for (std::size_t from = 0; from < graph.nodeCount(); from += 7) {
        for (std::size_t to = 0; to < graph.nodeCount(); to += 5) {
            const auto path = graph.shortestPath(from, to);
            if (!path) {
                withinTolerance &= reference.at(from, to) == DistanceTable::kUnreachable;
                continue;
            }
            const double diff = std::fabs(reference.at(from, to) - path->meters);
            const double allowed = 0.5 * static_cast<double>(path->nodes.size() - 1) + 1e-9;
            worst = std::max(worst, diff / std::max(allowed, 0.5));
            withinTolerance &= diff <= allowed;
        }
    }
    std::cout << "mayor diferencia con Dijkstra, como fracción de la tolerancia: " << worst << "\n";
    ok &= check("distancia entera - Dijkstra <= 0,5 m por calle del camino (redondeo de cada calle)", withinTolerance);
    return ok ? 0 : 1;
}
