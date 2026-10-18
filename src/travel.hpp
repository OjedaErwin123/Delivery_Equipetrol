#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "road_graph.hpp"

// El mapa de la simulación: el grafo, en qué nodo está cada restaurante, de dónde salen las
// motos y a qué velocidad andan. Se arma una vez antes de lanzar hilos (paramsFromConfig) y
// todos lo comparten con std::shared_ptr<const World>: nadie lo modifica, así que no hace falta
// lock, y el shared_ptr garantiza que vive mientras alguna moto lo use.
struct World {
    RoadGraph graph;
    std::unordered_map<std::string, std::size_t> restaurantNode;  // id del restaurante -> nodo
    std::size_t startNode = 0;
    double metersPerMs = 0;  // speedKmh / 3600
};

// Un viaje por las calles: el camino (nodos), la hora de salida y la velocidad. La moto lo
// publica una vez por viaje y duerme hasta llegar; la posición en cualquier instante es una
// función pura del tiempo (positionAt), así que quien la necesite (la ventana en cada frame,
// las estimaciones) la calcula sobre una copia del viaje, sin despertar a la moto.
// Un viaje de un solo nodo es una moto quieta en ese nodo.
class Trip {
public:
    // Quieta en `node` desde `sinceMs`.
    static Trip stay(std::size_t node, std::int64_t sinceMs);

    // Recorrer `path` (un camino del grafo) saliendo en `departMs` a `metersPerMs`. Tarda los
    // metros del camino divididos por la velocidad, redondeado hacia arriba a ms enteros.
    Trip(const RoadGraph& graph, const RoadGraph::Path& path, std::int64_t departMs, double metersPerMs);

    std::size_t from() const { return _nodes.front(); }
    std::size_t to() const { return _nodes.back(); }
    std::int64_t departMs() const { return _departMs; }
    std::int64_t arrivalMs() const { return _arrivalMs; }

    // Dónde está la moto en el instante `t`: antes de salir, en el primer nodo; después de
    // llegar, en el último; entre medio, sobre la calle que recorre, interpolada según los metros
    // andados. Nunca fuera de las calles del camino.
    Coordinates positionAt(const RoadGraph& graph, std::int64_t t) const;

private:
    Trip() = default;

    std::vector<std::size_t> _nodes;   // del origen al destino
    std::vector<double> _metersAt;     // metros desde el origen hasta cada nodo del camino
    std::int64_t _departMs = 0;
    std::int64_t _arrivalMs = 0;
    double _metersPerMs = 0;
};
