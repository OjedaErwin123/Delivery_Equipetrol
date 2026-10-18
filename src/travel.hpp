#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// Un lugar de la red: un nodo, o un punto a mitad de la calle a→b (una arista que existe), a
// `meters` metros de a. Una moto averiada queda a mitad de una calle: de ahí sale después, y ahí
// la van a buscar. Un nodo también es un lugar (constructor implícito a propósito: donde antes
// había un nodo, ahora puede haber un lugar).
struct Place {
    Place(std::size_t node = 0) : a(node), b(node) {}
    Place(std::size_t from, std::size_t to, double atMeters) : a(from), b(to), meters(atMeters) {}

    bool isNode() const { return a == b; }

    std::size_t a, b;
    double meters = 0;
};
bool operator==(const Place& x, const Place& y);
inline bool operator!=(const Place& x, const Place& y) { return !(x == y); }

// Las coordenadas de un lugar: sobre la recta de su calle (exacto a la escala de una calle).
Coordinates coordinatesOf(const RoadGraph& graph, const Place& place);
// Para mensajes: el id del nodo, o "a-b@metros".
std::string describe(const RoadGraph& graph, const Place& place);

// Un recorrido: desde un lugar hasta un nodo, o hasta un punto de una calle (`finish`). Si sale de
// mitad de una calle, primero termina esa calle hacia `path.nodes.front()` (hacia adelante, o hacia
// atrás si la calle es de doble sentido) y después sigue el camino mínimo; si termina a mitad de una
// calle, entra a ella desde `path.nodes.back()`. Sin nodos: los dos puntos están en la misma calle.
struct Route {
    Place start;
    RoadGraph::Path path;  // desde el nodo por el que sale de la calle (o `start` si es un nodo)
    double meters = 0;     // en total, incluidos los pedazos de calle del principio y del final
    std::optional<Place> finish;  // el punto de llegada, si no es un nodo
};

// El recorrido más corto de `from` a `to`, respetando el sentido único también en la calle donde
// está (por una calle a→b de sentido único solo se sale por b). std::nullopt si no hay camino.
std::optional<Route> routeTo(const RoadGraph& graph, const Place& from, std::size_t to);
// Lo mismo hasta un lugar cualquiera: a un punto de la calle a→b se entra por a, o también por b si
// es de doble sentido (o directo, si los dos están en la misma calle y el sentido lo permite).
std::optional<Route> routeTo(const RoadGraph& graph, const Place& from, const Place& to);

// Metros del recorrido más corto de `from` a `to` (routeTo). std::nullopt si no hay camino.
std::optional<double> metersBetween(const RoadGraph& graph, const Place& from, const Place& to);

// Un viaje por las calles: el recorrido, la hora de salida y la velocidad. La moto lo publica una
// vez por viaje y duerme hasta llegar; la posición en cualquier instante es una función pura del
// tiempo (positionAt), así que quien la necesite (la ventana en cada frame, las estimaciones) la
// calcula sobre una copia del viaje, sin despertar a la moto.
// Un viaje sin tramos es una moto quieta en un lugar.
class Trip {
public:
    // Quieta en `place` desde `sinceMs`.
    static Trip stay(const Place& place, std::int64_t sinceMs);

    // Recorrer `path` (un camino del grafo) saliendo en `departMs` a `metersPerMs`. Tarda los
    // metros del camino divididos por la velocidad, redondeado hacia arriba a ms enteros.
    Trip(const RoadGraph& graph, const RoadGraph::Path& path, std::int64_t departMs, double metersPerMs);
    // Lo mismo, desde un lugar cualquiera (p. ej. a mitad de una calle).
    Trip(const RoadGraph& graph, const Route& route, std::int64_t departMs, double metersPerMs);

    const Place& from() const { return _from; }
    const Place& to() const { return _to; }
    std::int64_t departMs() const { return _departMs; }
    std::int64_t arrivalMs() const { return _arrivalMs; }

    // Dónde está la moto en el instante `t`: antes de salir, en el origen; después de llegar, en
    // el destino; entre medio, sobre la calle que recorre según los metros andados. Nunca fuera
    // de las calles del recorrido.
    Place placeAt(std::int64_t t) const;
    // El mismo viaje, pero cortado en el instante `t`: llega (y queda) donde iba en `t`. Lo publica
    // una moto que se va a averiar en `t`: así nadie la ve pasar del punto de la avería.
    Trip until(std::int64_t t) const;
    Coordinates positionAt(const RoadGraph& graph, std::int64_t t) const;

private:
    Trip() = default;

    // Un pedazo del recorrido sobre la arista a→b: empieza a `offset` metros de a y anda `length`
    // metros hacia b. Una calle de doble sentido recorrida "hacia atrás" usa su arista b→a.
    struct Segment {
        std::size_t a, b;
        double offset, length, street;  // street: largo de la calle a-b
    };
    void add(const RoadGraph& graph, std::size_t a, std::size_t b, double offset, double length);

    Place _from, _to;
    std::vector<Segment> _segments;
    std::vector<double> _metersAt;  // metros desde el origen hasta el inicio de cada pedazo (+ el total)
    std::int64_t _departMs = 0;
    std::int64_t _arrivalMs = 0;
    double _metersPerMs = 0;
};
