#include "travel.hpp"

#include <algorithm>
#include <cmath>

Trip Trip::stay(std::size_t node, std::int64_t sinceMs)
{
    Trip trip;
    trip._nodes = {node};
    trip._metersAt = {0};
    trip._departMs = trip._arrivalMs = sinceMs;
    return trip;
}

Trip::Trip(const RoadGraph& graph, const RoadGraph::Path& path, std::int64_t departMs, double metersPerMs)
    : _nodes(path.nodes), _departMs(departMs), _metersPerMs(metersPerMs)
{
    // Los mismos metros que usa Dijkstra: haversine entre los extremos de cada calle.
    _metersAt.push_back(0);
    for (std::size_t i = 1; i < _nodes.size(); ++i) {
        const Coordinates& a = graph.coordinates(_nodes[i - 1]);
        const Coordinates& b = graph.coordinates(_nodes[i]);
        _metersAt.push_back(_metersAt.back() + haversineMeters(a.lat, a.lon, b.lat, b.lon));
    }
    _arrivalMs = _departMs + static_cast<std::int64_t>(std::ceil(_metersAt.back() / _metersPerMs));
}

Coordinates Trip::positionAt(const RoadGraph& graph, std::int64_t t) const
{
    if (t <= _departMs || _nodes.size() == 1) {
        return graph.coordinates(_nodes.front());
    }
    if (t >= _arrivalMs) {
        return graph.coordinates(_nodes.back());
    }
    const double meters = static_cast<double>(t - _departMs) * _metersPerMs;
    // La calle en la que va: la primera cuyo extremo final está más allá de lo andado.
    const auto next = std::upper_bound(_metersAt.begin() + 1, _metersAt.end(), meters);
    if (next == _metersAt.end()) {
        return graph.coordinates(_nodes.back());  // por redondeo: ya llegó
    }
    const std::size_t i = static_cast<std::size_t>(next - _metersAt.begin());
    const double length = _metersAt[i] - _metersAt[i - 1];
    const double f = length > 0 ? (meters - _metersAt[i - 1]) / length : 1.0;
    const Coordinates& a = graph.coordinates(_nodes[i - 1]);
    const Coordinates& b = graph.coordinates(_nodes[i]);
    // Interpolar en grados es exacto a la escala de una calle (decenas o cientos de metros).
    return {a.lat + f * (b.lat - a.lat), a.lon + f * (b.lon - a.lon)};
}
