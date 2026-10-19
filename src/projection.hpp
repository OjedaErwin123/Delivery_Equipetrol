#pragma once

#include "road_graph.hpp"

// De coordenadas del mapa (grados) a píxeles de la imagen. Los `bounds` de la configuración son los
// bordes de la imagen (norte arriba, oeste a la izquierda); a la escala de un barrio la proyección
// lineal coincide con la de las teselas de OSM. No depende de OpenCV: se prueba sin pantalla.
struct MapBounds {
    double north = 0, south = 0, west = 0, east = 0;
};

struct Pixel {
    double x = 0, y = 0;
};

inline Pixel toPixel(const MapBounds& b, int width, int height, const Coordinates& c)
{
    return {(c.lon - b.west) / (b.east - b.west) * width, (b.north - c.lat) / (b.north - b.south) * height};
}
