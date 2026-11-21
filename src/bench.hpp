#pragma once

#include <ostream>
#include <string>

#include "simulation.hpp"

// EC2: el modo `--bench` de delivery_sim. Mide cada aplicación de GPU sobre los datos de la configuración (la
// ciudad: config/santa_cruz.json) en un barrido de tamaños, con sus versiones:
//   cpu_1          la versión secuencial (la referencia de la prueba de igualdad);
//   cpu_threads    la multihilo con 1, 2, 4 … hasta hardware_concurrency() hilos (columna threads);
//   gpu_kernel     solo los kernels, con los datos ya en la GPU (eventos de CUDA);
//   gpu_end_to_end copias de ida y vuelta + kernels (la memoria se reserva una sola vez, al crear);
//   gpu_via_owner  lo que ve el hilo que pide: encolar en el hilo dueño de la GPU + todo lo anterior.
// Las versiones de GPU solo existen con USE_CUDA=ON. Cada tamaño: una corrida de calentamiento de cada versión
// (la primera llamada a CUDA paga el contexto) y después `repetitions` rondas que intercalan las versiones (el
// calor de una laptop no castiga siempre a la misma). Cada corrida se compara con la referencia (columna
// correct). Mismos datos y semilla fija para todas las versiones.
//
// Escribe en `csv` una línea por corrida: application,version,threads,size,repetition,ms,correct,machine
// y en `progress` una línea por tamaño con las medianas. Devuelve 0 si todas las corridas dieron lo correcto.
constexpr int kBenchRepetitions = 15;  // el documento pide al menos 10 corridas medidas

// `only`: el nombre de una aplicación (r1_nearest, t1_apsp, …) para medir solo esa; vacío = todas. Un nombre que
// no existe lanza std::invalid_argument con la lista de las que hay.
int runBenchmarks(const SimParams& params, std::ostream& csv, std::ostream& progress, int repetitions,
                  const std::string& only = "");
