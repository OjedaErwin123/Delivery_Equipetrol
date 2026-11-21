#pragma once

#include <cstddef>
#include <memory>

#include "diffusion.hpp"

// EC2, S2 en la GPU: los pasos de Jacobi de 5 puntos de la difusión de la demanda (ver diffusion.hpp), un
// lanzamiento por paso alternando dos buffers que se quedan en la GPU. Dos kernels (carpeta 06):
//   básico      cada hilo lee sus 4 vecinos de la memoria global;
//   coarsening  cada hilo calcula kRowsPerThread celdas de la misma columna, una debajo de la otra, y reusa en
//               registros los valores de arriba y del medio (register tiling, como el coarsening en z del curso):
//               3 lecturas de la columna por celda bajan a ~1. Es la versión que se usa.
// Flotantes: ver la tolerancia en diffusion.hpp.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class DiffusionGpu {
public:
    explicit DiffusionGpu(std::size_t maxCells);
    ~DiffusionGpu();
    DiffusionGpu(const DiffusionGpu&) = delete;
    DiffusionGpu& operator=(const DiffusionGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    Grid diffuse(const Grid& start, int steps, bool coarsened, Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _maxCells;
};
