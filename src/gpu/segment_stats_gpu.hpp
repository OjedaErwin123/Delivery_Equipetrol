#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "segment_stats.hpp"

// EC2, R2 en la GPU: la reducción segmentada por restaurante (ver segment_stats.hpp). Un bloque por restaurante: sus
// hilos recorren el grupo con paso de bloque (cada hilo acumula cantidad, suma y máximo en registros) y después
// el bloque reduce los tres valores en árbol en memoria compartida (carpeta 08). Sin atómicos: cada bloque escribe
// solo su restaurante. Enteros: exactamente lo mismo que la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class SegmentStatsGpu {
public:
    SegmentStatsGpu(std::size_t capacity, std::size_t maxRestaurants);
    ~SegmentStatsGpu();
    SegmentStatsGpu(const SegmentStatsGpu&) = delete;
    SegmentStatsGpu& operator=(const SegmentStatsGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    std::vector<RestaurantStats> stats(const Grouping& groups, const std::vector<std::uint32_t>& deliveryMs,
                                       Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity, _maxRestaurants;
};
