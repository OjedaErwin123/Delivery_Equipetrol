#include "travel.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

bool operator==(const Place& x, const Place& y)
{
    return x.a == y.a && x.b == y.b && x.meters == y.meters;
}

Coordinates coordinatesOf(const RoadGraph& graph, const Place& place)
{
    const Coordinates& a = graph.coordinates(place.a);
    if (place.isNode()) {
        return a;
    }
    const Coordinates& b = graph.coordinates(place.b);
    const double street = graph.streetMeters(place.a, place.b);
    const double f = street > 0 ? place.meters / street : 1.0;
    // Interpolar en grados es exacto a la escala de una calle (decenas o cientos de metros).
    return {a.lat + f * (b.lat - a.lat), a.lon + f * (b.lon - a.lon)};
}

std::string describe(const RoadGraph& graph, const Place& place)
{
    if (place.isNode()) {
        return graph.nodeId(place.a);
    }
    return graph.nodeId(place.a) + "-" + graph.nodeId(place.b) + "@" +
           std::to_string(static_cast<long long>(place.meters)) + "m";
}

std::optional<Route> routeTo(const RoadGraph& graph, const Place& from, std::size_t to)
{
    if (from.isNode()) {
        std::optional<RoadGraph::Path> path = graph.shortestPath(from.a, to);
        if (!path) {
            return std::nullopt;
        }
        const double meters = path->meters;
        return Route{from, std::move(*path), meters};
    }
    // A mitad de la calle a→b: sale por b (la arista a→b existe: por ahí venía) o, si la calle es
    // de doble sentido, también por a. Gana el más corto; en empate, hacia adelante.
    std::optional<Route> best;
    const auto consider = [&](std::size_t exit, double untilExit) {
        std::optional<RoadGraph::Path> path = graph.shortestPath(exit, to);
        if (path && (!best || untilExit + path->meters < best->meters)) {
            const double meters = untilExit + path->meters;
            best = Route{from, std::move(*path), meters};
        }
    };
    consider(from.b, graph.streetMeters(from.a, from.b) - from.meters);
    if (graph.hasStreet(from.b, from.a)) {
        consider(from.a, from.meters);
    }
    return best;
}

Trip Trip::stay(const Place& place, std::int64_t sinceMs)
{
    Trip trip;
    trip._from = trip._to = place;
    trip._metersAt = {0};
    trip._departMs = trip._arrivalMs = sinceMs;
    return trip;
}

Trip::Trip(const RoadGraph& graph, const RoadGraph::Path& path, std::int64_t departMs, double metersPerMs)
    : Trip(graph, Route{Place(path.nodes.front()), path, path.meters}, departMs, metersPerMs)
{
}

Trip::Trip(const RoadGraph& graph, const Route& route, std::int64_t departMs, double metersPerMs)
    : _from(route.start), _to(route.path.nodes.back()), _departMs(departMs), _metersPerMs(metersPerMs)
{
    _metersAt.push_back(0);
    const std::vector<std::size_t>& nodes = route.path.nodes;
    if (!_from.isNode()) {
        // El pedazo de la calle donde está: hacia adelante (a→b) o hacia atrás (por la arista b→a).
        const double street = graph.streetMeters(_from.a, _from.b);
        if (nodes.front() == _from.b) {
            add(graph, _from.a, _from.b, _from.meters, street - _from.meters);
        } else {
            add(graph, _from.b, _from.a, street - _from.meters, _from.meters);
        }
    }
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        add(graph, nodes[i - 1], nodes[i], 0, graph.streetMeters(nodes[i - 1], nodes[i]));
    }
    _arrivalMs = _departMs + static_cast<std::int64_t>(std::ceil(_metersAt.back() / _metersPerMs));
}

void Trip::add(const RoadGraph& graph, std::size_t a, std::size_t b, double offset, double length)
{
    _segments.push_back({a, b, offset, length, graph.streetMeters(a, b)});
    _metersAt.push_back(_metersAt.back() + length);
}

Place Trip::placeAt(std::int64_t t) const
{
    if (_segments.empty() || t <= _departMs) {
        return _from;
    }
    if (t >= _arrivalMs) {
        return _to;
    }
    const double meters = static_cast<double>(t - _departMs) * _metersPerMs;
    // El pedazo en el que va: el primero cuyo final está más allá de lo andado.
    const auto next = std::upper_bound(_metersAt.begin() + 1, _metersAt.end(), meters);
    if (next == _metersAt.end()) {
        return _to;  // por redondeo: ya llegó
    }
    const Segment& s = _segments[static_cast<std::size_t>(next - _metersAt.begin()) - 1];
    const double along = s.offset + (meters - *(next - 1));
    if (along <= 0) {
        return Place(s.a);
    }
    if (along >= s.street) {
        return Place(s.b);
    }
    return Place(s.a, s.b, along);
}

Trip Trip::until(std::int64_t t) const
{
    Trip cut = *this;
    cut._to = placeAt(t);
    cut._arrivalMs = std::max(t, _departMs);
    return cut;
}

Coordinates Trip::positionAt(const RoadGraph& graph, std::int64_t t) const
{
    return coordinatesOf(graph, placeAt(t));
}
