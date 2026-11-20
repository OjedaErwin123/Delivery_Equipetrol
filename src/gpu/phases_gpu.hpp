#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "phases.hpp"

// EC2, P1 en la GPU: las fases de los pedidos (ver phases.hpp) con los datos en las dos disposiciones (carpeta 04):
//   AoS  los registros de 32 bytes: en cada lectura, los 32 hilos de un warp leen campos separados por 32 bytes, así
//        que el warp trae 1 KB de la memoria para usar 16 bytes por hilo (la mitad de lo que trae se descarta);
//   SoA  un arreglo por campo: el warp lee 4 x 128 bytes contiguos, exactamente lo que usa (coalescido). Es la
//        versión que se usa.
// Cada hilo acumula en registros (paso de grid), el bloque reduce en memoria compartida y suma una vez con
// atomicAdd de 64 bits. Enteros: exactamente lo mismo que la CPU. bytesRead() da los bytes que cada versión trae de
// la memoria global, para el ancho de banda efectivo.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class PhasesGpu {
public:
    explicit PhasesGpu(std::size_t capacity);
    ~PhasesGpu();
    PhasesGpu(const PhasesGpu&) = delete;
    PhasesGpu& operator=(const PhasesGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    PhaseTotals fromRecords(const std::vector<OrderRecord>& records, Timing* timing = nullptr);  // AoS
    PhaseTotals fromColumns(const OrderColumns& columns, Timing* timing = nullptr);              // SoA

    static std::size_t bytesRead(std::size_t orders, bool soa) { return orders * (soa ? 16 : sizeof(OrderRecord)); }

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _capacity;
};
