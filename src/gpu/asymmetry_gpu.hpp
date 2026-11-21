#pragma once

#include <cstddef>
#include <memory>

#include "asymmetry.hpp"

// EC2, P2 en la GPU: el rodeo de los sentidos únicos (ver asymmetry.hpp), con tres kernels de la carpeta 04. Un bloque
// de 32 x 8 hilos por bloque (I, J) de la tabla con J >= I (los de abajo de la diagonal salen enseguida); cada hilo
// hace 4 filas del bloque. La ida d[i][j] se lee por filas (un warp = 32 valores seguidos: coalescido) en los tres.
// La vuelta d[j][i]:
//   naive   directo de la memoria global: los 32 hilos de un warp leen 32 filas distintas (32 sectores por lectura,
//           sin coalescer; 01_coalescing_segments);
//   tiled   corner turning: el bloque copia el espejo (J, I) por filas a memoria compartida [32][32] y lo lee
//           transpuesto (03_corner_turning); pero leer una columna de [32][32] cae 32 veces en el mismo banco;
//   padded  lo mismo con [32][33]: la fila de relleno corre cada fila un banco y la columna cae en 32 bancos
//           distintos (05_bank_conflicts_padding). Es el que se usa.
// Cada hilo acumula en registros; el bloque reduce en memoria compartida y suma una vez al total (atomicAdd de 64
// bits; el peor par con atomicMax). Enteros: exactamente lo mismo que la CPU.
//
// No es seguro entre hilos: lo usa un solo hilo (el dueño de la GPU) o una prueba.
class AsymmetryGpu {
public:
    enum class Kernel { naive, tiled, padded };

    // Reserva la tabla para hasta `stride` x `stride` valores.
    explicit AsymmetryGpu(std::size_t stride);
    ~AsymmetryGpu();
    AsymmetryGpu(const AsymmetryGpu&) = delete;
    AsymmetryGpu& operator=(const AsymmetryGpu&) = delete;

    struct Timing {
        double kernelMs = 0;
        double endToEndMs = 0;
    };

    // Copia la tabla, lanza el kernel y trae los totales.
    Asymmetry compare(const DistanceTable& table, Kernel kernel, Timing* timing = nullptr);

private:
    struct Buffers;
    std::unique_ptr<Buffers> _d;
    std::size_t _stride;
};
