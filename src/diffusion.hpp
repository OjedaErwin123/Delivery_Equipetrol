#pragma once

#include <cstddef>
#include <vector>

#include "heatmap.hpp"
#include "thread_pool.hpp"

// EC2, S2 (stencil en flotantes): la difusión de la demanda en el tiempo. Partiendo de la grilla de pedidos (la de
// C1), cada paso de Jacobi de 5 puntos hace, para cada celda interior,
//     u'[c] = kKeep · ( u[c] + kSpread · (u[norte] + u[sur] + u[este] + u[oeste] - 4·u[c]) )
// la demanda se reparte hacia las celdas vecinas (kSpread) y se va apagando (kKeep < 1: lo que se pidió hace rato
// pesa menos). Dos buffers y bordes fijos en 0 (no entra demanda de afuera de la ciudad). Estable con kSpread < 1/4.
// Flotantes: la CPU y la GPU hacen las mismas operaciones en el mismo orden, pero la GPU usa FMA; después de
// `steps` pasos coinciden dentro de 1e-5 del valor máximo de la grilla inicial (diffusion_check y
// gpu_diffusion_check).
constexpr float kSpread = 0.2f;
constexpr float kKeep = 0.98f;

// CPU, 1 hilo.
Grid diffuse(const Grid& start, int steps);

// CPU, N hilos: cada paso reparte las filas entre los trabajos del pool y espera a todos antes del siguiente.
Grid diffuseParallel(const Grid& start, int steps, ThreadPool& pool);
