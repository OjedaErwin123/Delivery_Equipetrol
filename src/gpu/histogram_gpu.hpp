#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "histogram.hpp"

// EC2, H1 en la GPU: el histograma de los tiempos de entrega (cajas de 1 minuto, ver histogram.hpp), con dos
// kernels para comparar (carpeta 07):
//   global       cada hilo hace atomicAdd en el histograma de la memoria global: todos los hilos compiten por las
//                mismas 256 cajas (y por las pocas de los tiempos más comunes): contención;
//   privatizado  cada bloque cuenta en SU histograma en memoria compartida (atómicos de memoria compartida, entre
//                los hilos del bloque) y al final lo suma una vez al global: 256 atómicos globales por bloque en
//                lugar de uno por entrega. Es la versión que se usa.
// Conteos enteros: los dos dan exactamente lo mismo que la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class HistogramGpu {
public:
    // Reserva en la GPU lugar para `capacity` tiempos (una sola vez).
    explicit HistogramGpu(std::size_t capacity);
    ~HistogramGpu();
    HistogramGpu(const HistogramGpu&) = delete;
    HistogramGpu& operator=(const HistogramGpu&) = delete;

    struct Timing {
        double kernelMs = 0;    // el kernel (eventos de CUDA), con los datos ya en la GPU
        double endToEndMs = 0;  // copia de ida + kernel + copia de vuelta del histograma
    };

    Histogram privatized(const std::vector<std::uint32_t>& ms, Timing* timing = nullptr);
    Histogram globalAtomics(const std::vector<std::uint32_t>& ms, Timing* timing = nullptr);

    static unsigned blocksFor(std::size_t n);

private:
    Histogram run(const std::vector<std::uint32_t>& ms, bool privatize, Timing* timing);
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity;
};
