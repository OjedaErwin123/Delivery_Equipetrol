#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.hpp"

// Distancia en metros entre dos puntos (grados) sobre la esfera terrestre (fórmula de haversine).
// El formato de la configuración no trae el largo de las calles: sale de las coordenadas de sus nodos.
double haversineMeters(double lat1, double lon1, double lat2, double lon2);

// Un punto del mapa, en grados.
struct Coordinates {
    double lat = 0, lon = 0;
};

// La red vial: intersecciones conectadas por calles dirigidas.
//
// Una calle de doble sentido es dos aristas (ida y vuelta); una de sentido único, solo
// from -> to: así el camino mínimo respeta el sentido único. El peso de cada arista es su
// largo en metros; con una sola velocidad para toda la flota, el camino más corto es
// también el más rápido.
//
// Se construye una vez, en el hilo principal y antes de lanzar ningún hilo; después todos
// los métodos son const y nadie lo modifica: los hilos lo pueden leer a la vez sin lock y
// sin data race posible (como SimClock).
class RoadGraph {
public:
    // `config` ya está validada: cada calle y restaurante apunta a un nodo que existe.
    // Precalcula las distancias desde el nodo de cada restaurante (un Dijkstra por restaurante).
    explicit RoadGraph(const Config& config);

    struct Path {
        std::vector<std::size_t> nodes;  // índices de nodo, del origen al destino (ambos incluidos)
        double meters = 0;
    };

    std::size_t nodeCount() const { return _ids.size(); }
    const std::string& nodeId(std::size_t node) const { return _ids[node]; }
    std::optional<std::size_t> nodeIndex(const std::string& id) const;
    const Coordinates& coordinates(std::size_t node) const { return _coordinates[node]; }

    // Camino mínimo de `from` a `to` por las calles (Dijkstra). std::nullopt si no hay camino.
    std::optional<Path> shortestPath(std::size_t from, std::size_t to) const;

    // ¿Hay camino del restaurante `restaurant` (su posición en config.restaurants) al nodo
    // `node`? Consulta lo precalculado: no hace trabajo al crear un pedido.
    bool reachableFromRestaurant(std::size_t restaurant, std::size_t node) const;

private:
    struct Edge {
        std::size_t to;
        double meters;
    };
    struct Search {
        std::vector<double> meters;                         // infinito: no alcanzable
        std::vector<std::optional<std::size_t>> previous;   // nodo anterior en el camino mínimo
    };

    Search dijkstra(std::size_t from) const;

    std::vector<std::string> _ids;
    std::vector<Coordinates> _coordinates;
    std::unordered_map<std::string, std::size_t> _index;
    std::vector<std::vector<Edge>> _out;               // aristas que salen de cada nodo
    std::vector<std::vector<double>> _fromRestaurant;  // metros desde cada restaurante a cada nodo
};
