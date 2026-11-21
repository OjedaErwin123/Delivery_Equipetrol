#pragma once

#include <cstddef>
#include <cstdint>

#include "apsp.hpp"
#include "thread_pool.hpp"

// EC2, P2 (performance: acceso coalescido, corner turning y conflictos de bancos): cuánto rodeo imponen los sentidos
// únicos. Sobre la tabla de todos contra todos de T1 (metros enteros), para cada par de nodos i < j se compara la ida
// d[i][j] con la vuelta d[j][i]. Si las calles fueran todas de doble mano serían iguales; con sentido único, una de
// las dos puede dar la vuelta a la manzana.
//
// Leer d[i][j] recorriendo j es leer por filas; leer d[j][i] recorriendo j es leer la tabla POR COLUMNAS: cada lectura
// salta una fila entera. Es el problema que resuelve el "corner turning" (carpeta 04): traer el bloque espejo (J, I)
// por filas y leerlo transpuesto desde una memoria rápida (la caché de la CPU; la memoria compartida de la GPU).
//
// Enteros: la CPU y la GPU dan exactamente lo mismo (sumas de 64 bits y un máximo con desempate fijo).
struct Asymmetry {
    std::uint64_t pairs = 0;       // pares i < j con camino en los dos sentidos
    std::uint64_t oneWay = 0;      // pares i < j con camino en un solo sentido
    std::uint64_t asymmetric = 0;  // de `pairs`, con ida != vuelta
    std::uint64_t sumDiff = 0;     // suma de |ida - vuelta| sobre `pairs`, en metros
    // El peor par como una sola clave de 64 bits, para poder combinarlo con un máximo (atomicMax en la GPU):
    // (|ida - vuelta| << 32) | (0xffffffff - (i << 16 | j)). A igual diferencia gana el menor i y después el menor j.
    // 0 si no hay ningún par asimétrico.
    std::uint64_t worst = 0;

    std::uint32_t maxDiff() const { return static_cast<std::uint32_t>(worst >> 32); }
    std::size_t worstI() const { return (0xffffffffu - static_cast<std::uint32_t>(worst)) >> 16; }
    std::size_t worstJ() const { return (0xffffffffu - static_cast<std::uint32_t>(worst)) & 0xffffu; }

    bool operator==(const Asymmetry& o) const
    {
        return pairs == o.pairs && oneWay == o.oneWay && asymmetric == o.asymmetric && sumDiff == o.sumDiff &&
               worst == o.worst;
    }
    void add(const Asymmetry& o);  // combinar dos resultados parciales (sumas y máximo)
};

// Los índices del peor par van en 16 bits cada uno.
constexpr std::size_t kMaxAsymmetryNodes = 65535;

// CPU, 1 hilo, ingenua: i < j fila por fila; d[j][i] se lee por columnas (un salto de una fila por lectura).
Asymmetry asymmetryNaive(const DistanceTable& table);

// CPU, 1 hilo, por bloques de 32 x 32: cada bloque (I, J) con J >= I junto con su espejo (J, I); los dos (8 KB)
// caben en la caché L1, así que la lectura por columnas del espejo ya no va a la memoria.
Asymmetry asymmetryBlocked(const DistanceTable& table);

// CPU, N hilos: lo mismo por bloques, una fila de bloques (I) por trabajo, repartidas por la cola del pool (las filas
// de arriba tienen más bloques: la cola las reparte a medida que los hilos se liberan); sumas privadas por trabajo,
// combinadas al final (combiners, carpeta 10).
Asymmetry asymmetryParallel(const DistanceTable& table, ThreadPool& pool);

// Una tabla sintética de n nodos para el bench y las pruebas (semilla fija): ida = vuelta en ~65 % de los pares; en
// el resto, uno de los dos sentidos es más largo (hasta `maxExtra` metros); ~1 % de los pares con un solo sentido y
// ~0,5 % sin camino. Diferencias de pocos valores distintos (`maxExtra` chico) fuerzan empates en el peor par.
DistanceTable syntheticTable(std::size_t n, std::uint32_t seed, std::uint32_t maxExtra = 3000);
