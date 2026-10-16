// Prueba de la red vial: largo de las calles desde las coordenadas, camino mínimo con sentido
// único, nodos inalcanzables, empates y el mapa real. Uso: road_graph_check <config.json>
// Sale con 0 si todo está bien.
#include <cmath>
#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "log_line.hpp"
#include "road_graph.hpp"

namespace {

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

// Los ids de nodo del camino, para comparar con lo esperado.
std::vector<std::string> ids(const RoadGraph& graph, const RoadGraph::Path& path)
{
    std::vector<std::string> result;
    for (std::size_t node : path.nodes) {
        result.push_back(graph.nodeId(node));
    }
    return result;
}

// ¿Cada paso del camino es una calle que existe y se recorre en un sentido permitido?
bool followsStreets(const Config& config, const RoadGraph& graph, const RoadGraph::Path& path)
{
    std::set<std::pair<std::string, std::string>> allowed;
    for (const StreetConfig& s : config.streets) {
        allowed.emplace(s.from, s.to);
        if (!s.oneWay) {
            allowed.emplace(s.to, s.from);
        }
    }
    for (std::size_t i = 1; i < path.nodes.size(); ++i) {
        if (allowed.count({graph.nodeId(path.nodes[i - 1]), graph.nodeId(path.nodes[i])}) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    bool ok = true;

    // 1. Un grado de latitud mide R·π/180 ≈ 111 195 m.
    ok &= check("haversine: un grado de latitud", near(haversineMeters(0, 0, 1, 0), 111194.93, 1.0));
    ok &= check("haversine: mismo punto = 0", haversineMeters(-17.77, -63.19, -17.77, -63.19) == 0.0);

    // 2. Grafo a mano. a→b→c es de sentido único y más corto (~222 m); a–d–c es de doble
    //    sentido y más largo (~314 m). e no tiene calles.
    Config c;
    c.nodes = {{"a", 0, 0}, {"b", 0, 0.001}, {"c", 0, 0.002}, {"d", 0.001, 0.001}, {"e", 0.002, 0.002}};
    c.streets = {{"s0", "a", "b", true}, {"s1", "b", "c", true}, {"s2", "a", "d", false}, {"s3", "d", "c", false}};
    c.restaurants = {{"r0", "R0", "b", 1, 0, 0}, {"r1", "R1", "e", 1, 0, 0}};
    const RoadGraph g(c);
    const std::size_t a = *g.nodeIndex("a"), cc = *g.nodeIndex("c"), e = *g.nodeIndex("e");

    const auto ac = g.shortestPath(a, cc);
    const double ab = haversineMeters(0, 0, 0, 0.001);
    ok &= check("a→c por el camino más corto (sentido único a favor)",
                ac && ids(g, *ac) == std::vector<std::string>{"a", "b", "c"} && near(ac->meters, 2 * ab, 1e-6));
    const auto ca = g.shortestPath(cc, a);
    ok &= check("c→a no usa la calle de sentido único al revés",
                ca && ids(g, *ca) == std::vector<std::string>{"c", "d", "a"} && ca->meters > ac->meters);
    const auto aa = g.shortestPath(a, a);
    ok &= check("origen = destino: camino de un nodo y 0 m", aa && aa->nodes.size() == 1 && aa->meters == 0);
    ok &= check("nodo sin calles: sin camino", !g.shortestPath(a, e) && !g.shortestPath(e, a));
    ok &= check("alcanzable desde r0 (en b): c y, dando la vuelta, a",
                g.reachableFromRestaurant(0, cc) && g.reachableFromRestaurant(0, a));
    ok &= check("no alcanzable: e desde r0; nada desde r1 (en e) salvo e",
                !g.reachableFromRestaurant(0, e) && !g.reachableFromRestaurant(1, a) &&
                    g.reachableFromRestaurant(1, e));
    ok &= check("id inexistente", !g.nodeIndex("zz"));

    // 3. Empate: x e y están a la misma distancia de a y de t. Gana el de menor índice (x),
    //    siempre: el camino no depende del azar ni del compilador.
    Config t;
    t.nodes = {{"a", 0, 0}, {"x", 0.001, 0.001}, {"y", -0.001, 0.001}, {"t", 0, 0.002}};
    t.streets = {{"s0", "a", "y", false}, {"s1", "a", "x", false}, {"s2", "y", "t", false}, {"s3", "x", "t", false}};
    const RoadGraph tg(t);
    const auto at = tg.shortestPath(*tg.nodeIndex("a"), *tg.nodeIndex("t"));
    ok &= check("empate: gana el nodo de menor índice", at && ids(tg, *at) == std::vector<std::string>{"a", "x", "t"});

    // 4. El mapa real.
    if (argc < 2) {
        std::cerr << "uso: road_graph_check <config.json>\n";
        return 1;
    }
    const Config real = loadConfig(argv[1]);
    const RoadGraph rg(real);
    const std::size_t start = *rg.nodeIndex(real.startNode);
    bool startReachesAll = true;
    bool pathsFollowStreets = true;
    for (std::size_t r = 0; r < real.restaurants.size(); ++r) {
        const std::size_t node = *rg.nodeIndex(real.restaurants[r].node);
        const auto toRestaurant = rg.shortestPath(start, node);
        startReachesAll = startReachesAll && toRestaurant;
        pathsFollowStreets = pathsFollowStreets && toRestaurant && followsStreets(real, rg, *toRestaurant);
        std::size_t reachable = 0;
        for (std::size_t n = 0; n < rg.nodeCount(); ++n) {
            reachable += rg.reachableFromRestaurant(r, n) ? 1 : 0;
        }
        std::cout << "  " << real.restaurants[r].id << " (" << real.restaurants[r].node << "): desde "
                  << real.startNode << " " << (toRestaurant ? std::lround(toRestaurant->meters) : -1)
                  << " m; alcanza " << reachable << " de " << rg.nodeCount() << " nodos\n";
    }
    ok &= check("mapa real: el grafo tiene todos los nodos", rg.nodeCount() == real.nodes.size());
    ok &= check("mapa real: cada restaurante es alcanzable desde startNode", startReachesAll);
    ok &= check("mapa real: los caminos siguen calles en sentido permitido", pathsFollowStreets);
    return ok ? 0 : 1;
}
