#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "coverage.hpp"

// EC2, S1 en la GPU: los barridos de Jacobi del mapa de cobertura (ver coverage.hpp), un lanzamiento por barrido
// alternando los dos buffers (el fin de cada lanzamiento es la barrera entre barridos: ningún bloque lee lo que otro
// está escribiendo). Dos kernels (carpeta 06):
//   básico      cada hilo (una celda) lee sus 4 vecinos de la memoria global;
//   por bloques un bloque de 32 x 8 hilos carga su bloque con un halo de 1 celda en memoria compartida: cada valor se
//               lee de la memoria global una vez y lo usan hasta 5 celdas. Es la versión que se usa.
// Los datos se quedan en la GPU entre barridos: se copian una vez de ida y una de vuelta. Enteros: exactamente lo
// mismo que la CPU con el mismo número de barridos.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class CoverageGpu {
public:
    explicit CoverageGpu(std::size_t maxCells);
    ~CoverageGpu();
    CoverageGpu(const CoverageGpu&) = delete;
    CoverageGpu& operator=(const CoverageGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    std::vector<std::uint32_t> relax(const CoverageGrid& grid, int sweeps, bool tiled, Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _maxCells;
};
