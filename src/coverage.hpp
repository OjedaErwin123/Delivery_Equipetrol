#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "thread_pool.hpp"

// EC2, S1 (stencil): el mapa de cobertura. Sobre una grilla de la ciudad, cuánto cuesta llegar desde cada celda al
// restaurante más cercano: las celdas con un restaurante valen 0; pasar por una celda con calles cuesta
// kStreetCost y por una sin calles kOffStreetCost (un rodeo). Cada barrido de Jacobi hace, para cada celda interior,
//     d'[c] = min(d[c], min(d de sus 4 vecinos) + costo[c])
// con DOS buffers (se lee uno y se escribe el otro) y los bordes fijos (las celdas del borde no se actualizan: valen
// lo que valían; nadie llega por afuera de la ciudad). Después de suficientes barridos (el largo del camino más
// largo) el campo ya no cambia. Enteros: la CPU y la GPU, con el mismo número de barridos, dan exactamente lo mismo.
constexpr std::uint32_t kStreetCost = 1;
constexpr std::uint32_t kOffStreetCost = 3;
constexpr std::uint32_t kFar = 0x3fffffffu;  // "todavía no se llega": dos sumados entran en 32 bits

struct CoverageGrid {
    int width = 0, height = 0;
    std::vector<std::uint32_t> cost;      // costo de pasar por cada celda
    std::vector<std::uint32_t> distance;  // el campo: 0 en los restaurantes, kFar en el resto al empezar
};

// CPU, 1 hilo: `sweeps` barridos de Jacobi.
std::vector<std::uint32_t> relaxCoverage(const CoverageGrid& grid, int sweeps);

// CPU, N hilos: cada barrido reparte las filas en tramos entre los trabajos del pool (strips por filas) y espera a
// todos antes del siguiente (los dos buffers: nadie lee lo que otro está escribiendo en el mismo barrido).
std::vector<std::uint32_t> relaxCoverageParallel(const CoverageGrid& grid, int sweeps, ThreadPool& pool);
