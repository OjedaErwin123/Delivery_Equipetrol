#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "slot_histogram.hpp"

// EC2, H2 en la GPU: el histograma 2D (restaurante, franja de 10 minutos) de los pedidos creados (ver
// slot_histogram.hpp), privatizado en memoria compartida (hasta 64 x 144 cajas = 36 KB por bloque), con las dos
// formas de coarsening de la carpeta 07 para comparar:
//   intercalada  cada hilo recorre i, i + gridDim·blockDim, …: en cada vuelta los 32 hilos de un warp leen 32
//                pedidos seguidos (accesos coalescidos). Es la versión que se usa;
//   contigua     cada hilo recorre su propio tramo contiguo: en cada vuelta los hilos de un warp leen posiciones
//                separadas por el largo del tramo (no coalescidas).
// Conteos enteros: las dos dan exactamente lo mismo que la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class SlotHistogramGpu {
public:
    explicit SlotHistogramGpu(std::size_t capacity);
    ~SlotHistogramGpu();
    SlotHistogramGpu(const SlotHistogramGpu&) = delete;
    SlotHistogramGpu& operator=(const SlotHistogramGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    SlotHistogram count(const std::vector<std::uint32_t>& restaurantOf, const std::vector<std::uint32_t>& createdMs,
                        std::size_t restaurants, bool interleaved, Timing* timing = nullptr);

    static unsigned blocksFor(std::size_t n);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity;
};
