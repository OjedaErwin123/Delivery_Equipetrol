#include "road_graph.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <future>
#include <limits>
#include <queue>
#include <utility>

namespace {

constexpr double kEarthRadiusM = 6371000.0;  // radio medio de la Tierra
constexpr double kInfinity = std::numeric_limits<double>::infinity();

double radians(double degrees) { return degrees * 3.14159265358979323846 / 180.0; }

}  // namespace

double haversineMeters(double lat1, double lon1, double lat2, double lon2)
{
    const double dLat = radians(lat2 - lat1);
    const double dLon = radians(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(radians(lat1)) * std::cos(radians(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * kEarthRadiusM * std::asin(std::sqrt(a));
}

RoadGraph::RoadGraph(const Config& config) : _out(config.nodes.size())
{
    for (std::size_t i = 0; i < config.nodes.size(); ++i) {
        _ids.push_back(config.nodes[i].id);
        _coordinates.push_back({config.nodes[i].lat, config.nodes[i].lon});
        _index.emplace(config.nodes[i].id, i);
    }
    for (const StreetConfig& street : config.streets) {
        const std::size_t from = _index.at(street.from);
        const std::size_t to = _index.at(street.to);
        const NodeConfig& a = config.nodes[from];
        const NodeConfig& b = config.nodes[to];
        const double meters = haversineMeters(a.lat, a.lon, b.lat, b.lon);
        _out[from].push_back(Edge{to, meters});
        if (!street.oneWay) {
            _out[to].push_back(Edge{from, meters});
        }
    }
    // Un Dijkstra desde cada restaurante, todos a la vez: std::async con política EXPLÍCITA
    // (std::launch::async: cada uno en su hilo ya, no diferido hasta el get). Aquí sí sirve
    // std::async: se esperan todos antes de seguir, así que el future que bloquea al destruirse
    // no molesta (para las estimaciones del despachador, en cambio, se usa el pool).
    // Sin data race: dijkstra() solo LEE _out, que ya está completo, y _fromRestaurant se escribe
    // después, en este hilo, con los resultados de get() (que además relanza si algo falló).
    std::vector<std::future<std::vector<double>>> searches;
    for (const RestaurantConfig& restaurant : config.restaurants) {
        const std::size_t node = _index.at(restaurant.node);
        searches.push_back(std::async(std::launch::async, [this, node] { return dijkstra(node).meters; }));
    }
    for (std::future<std::vector<double>>& search : searches) {
        _fromRestaurant.push_back(search.get());
    }
}

std::optional<std::size_t> RoadGraph::nodeIndex(const std::string& id) const
{
    const auto it = _index.find(id);
    if (it == _index.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RoadGraph::hasStreet(std::size_t from, std::size_t to) const
{
    return std::any_of(_out[from].begin(), _out[from].end(), [to](const Edge& edge) { return edge.to == to; });
}

double RoadGraph::streetMeters(std::size_t a, std::size_t b) const
{
    return haversineMeters(_coordinates[a].lat, _coordinates[a].lon, _coordinates[b].lat, _coordinates[b].lon);
}

std::optional<RoadGraph::Path> RoadGraph::shortestPath(std::size_t from, std::size_t to) const
{
    const Search search = dijkstra(from);
    if (search.meters[to] == kInfinity) {
        return std::nullopt;
    }
    Path path;
    path.meters = search.meters[to];
    for (std::optional<std::size_t> node = to; node; node = search.previous[*node]) {
        path.nodes.push_back(*node);
    }
    std::reverse(path.nodes.begin(), path.nodes.end());
    return path;
}

bool RoadGraph::reachableFromRestaurant(std::size_t restaurant, std::size_t node) const
{
    return _fromRestaurant[restaurant][node] != kInfinity;
}

// Dijkstra con cola de prioridad y "borrado perezoso": un nodo puede entrar varias veces a
// la cola; al sacarlo se ignora si ya se conoce una distancia menor. Los empates se resuelven
// por índice de nodo (la cola ordena pares (metros, nodo)), así el camino es siempre el mismo.
RoadGraph::Search RoadGraph::dijkstra(std::size_t from) const
{
    Search search;
    search.meters.assign(_out.size(), kInfinity);
    search.previous.assign(_out.size(), std::nullopt);

    using Entry = std::pair<double, std::size_t>;  // (metros desde el origen, nodo)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    search.meters[from] = 0;
    queue.emplace(0.0, from);
    while (!queue.empty()) {
        const auto [meters, node] = queue.top();
        queue.pop();
        if (meters > search.meters[node]) {
            continue;  // entrada vieja: ya se llegó a este nodo por un camino más corto
        }
        for (const Edge& edge : _out[node]) {
            const double candidate = meters + edge.meters;
            if (candidate < search.meters[edge.to]) {
                search.meters[edge.to] = candidate;
                search.previous[edge.to] = node;
                queue.emplace(candidate, edge.to);
            }
        }
    }
    return search;
}
