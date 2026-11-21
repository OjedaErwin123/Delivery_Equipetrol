#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "apsp.hpp"

// EC2, T1 en la GPU: Floyd–Warshall por bloques (tiling, carpeta 03). La tabla se parte en bloques de 32 × 32; por
// cada bloque diagonal kb hay tres fases:
//   1. el bloque (kb, kb) contra sí mismo (un bloque de hilos);
//   2. los bloques de la fila kb y de la columna kb, cada uno contra el diagonal (2 · (nb - 1) bloques de hilos);
//   3. todos los demás, cada uno con su bloque de la fila kb y su bloque de la columna kb (nb² bloques de hilos).
// Cada fase carga sus bloques en memoria compartida y los reusa 32 veces: 32 × 32 hilos leen 2 · 32 · 32 valores
// de la memoria global y hacen 32 · 32 · 32 min-plus (en lugar de leer dos valores por cada min-plus).
// Enteros: el resultado es exactamente el de floydWarshall en la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba. La tabla queda en la GPU
// después de calcularla (para otras aplicaciones).
class ApspGpu {
public:
    // Reserva en la GPU una tabla de stride × stride (stride múltiplo de 32) una sola vez.
    explicit ApspGpu(std::size_t maxStride);
    ~ApspGpu();
    ApspGpu(const ApspGpu&) = delete;
    ApspGpu& operator=(const ApspGpu&) = delete;

    struct Timing {
        double kernelMs = 0;     // las 3 · nb fases (eventos de CUDA)
        double endToEndMs = 0;   // copia de ida de la tabla + fases + copia de vuelta
    };

    // Resuelve `table` (la inicial) en el lugar.
    void solve(DistanceTable& table, Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _maxStride;
};
