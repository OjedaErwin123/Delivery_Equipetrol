#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "heatmap.hpp"

// EC2, C1 en la GPU: la convolución gaussiana del mapa de calor (ver heatmap.hpp). El filtro (169 coeficientes) va en
// MEMORIA CONSTANTE: todos los hilos de un warp leen el mismo coeficiente a la vez y la caché constante lo reparte en
// un solo acceso. Dos kernels (carpeta 05):
//   básico     cada hilo (una celda de salida) lee sus 169 vecinos de la memoria global;
//   por bloques un bloque de 16 x 16 hilos carga UNA vez su bloque de entrada de 28 x 28 (los 16 x 16 más el halo de
//              6 de cada lado) en memoria compartida y las 169 lecturas de cada celda salen de ahí. Es la versión
//              que se usa.
// Flotantes con la misma suma en el mismo orden que la CPU (con FMA): ver la tolerancia en heatmap.hpp.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class HeatmapGpu {
public:
    explicit HeatmapGpu(std::size_t maxCells);
    ~HeatmapGpu();
    HeatmapGpu(const HeatmapGpu&) = delete;
    HeatmapGpu& operator=(const HeatmapGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    Grid convolve(const Grid& in, const std::vector<float>& filter, bool tiled, Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _maxCells;
};
