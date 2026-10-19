// Prueba de la proyección de la ventana (sin pantalla ni OpenCV): las esquinas de los bounds caen en
// las esquinas de la imagen, el norte arriba y el oeste a la izquierda, y un punto del medio en el
// medio. Sale con 0 si todo está bien.
#include <cmath>
#include <iostream>

#include "log_line.hpp"
#include "projection.hpp"

namespace {

bool near(const Pixel& p, double x, double y) { return std::fabs(p.x - x) < 1e-6 && std::fabs(p.y - y) < 1e-6; }

}  // namespace

int main()
{
    bool ok = true;
    // Los bounds de config/equipetrol.json y el tamaño de data/equipetrol.png.
    const MapBounds b{-17.7537957, -17.7745163, -63.2060194, -63.1859994};
    const int w = 933, h = 1014;
    ok &= check("esquina noroeste = (0, 0)", near(toPixel(b, w, h, {b.north, b.west}), 0, 0));
    ok &= check("esquina sureste = (ancho, alto)", near(toPixel(b, w, h, {b.south, b.east}), w, h));
    ok &= check("el centro de los bounds, en el centro de la imagen",
                near(toPixel(b, w, h, {(b.north + b.south) / 2, (b.west + b.east) / 2}), w / 2.0, h / 2.0));
    const Pixel n0 = toPixel(b, w, h, {-17.7649293, -63.1958985});  // n0, la rotonda de San Martín
    std::cout << "n0 en (" << n0.x << ", " << n0.y << ")\n";
    ok &= check("un nodo de la red cae dentro de la imagen", n0.x > 0 && n0.x < w && n0.y > 0 && n0.y < h);
    return ok ? 0 : 1;
}
