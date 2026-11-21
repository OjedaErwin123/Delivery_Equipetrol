#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "grouping.hpp"

// EC2, SC1 en la GPU: el counting sort estable de los pedidos por restaurante (ver grouping.hpp), en tres kernels:
//   1. countBlocks: cada bloque tiene un tramo contiguo de pedidos y cada hilo un sub-tramo contiguo de ese tramo;
//      cada hilo cuenta lo suyo en SU columna de una tabla [restaurante][hilo] en memoria compartida (sin
//      atómicos) y el bloque deja sus totales por restaurante;
//   2. scanCounts: un scan exclusivo (Kogge–Stone, carpeta 09) sobre los totales en orden (restaurante, bloque):
//      dónde empieza cada bloque en el grupo de cada restaurante;
//   3. scatterStable: cada bloque vuelve a contar, hace el scan de su tabla por hilos y cada hilo reparte su
//      sub-tramo en orden. Estable: los sub-tramos son contiguos y están ordenados por hilo y por bloque.
// Enteros y orden estable: exactamente lo mismo que la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class GroupingGpu {
public:
    static constexpr std::size_t kMaxRestaurants = 64;  // la tabla [64][128] de 4 bytes = 32 KB de memoria compartida

    explicit GroupingGpu(std::size_t capacity);
    ~GroupingGpu();
    GroupingGpu(const GroupingGpu&) = delete;
    GroupingGpu& operator=(const GroupingGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    Grouping group(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants, Timing* timing = nullptr);

    static unsigned blocksFor(std::size_t n);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity;
};
