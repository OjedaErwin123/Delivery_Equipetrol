#pragma once

#include <cstddef>
#include <vector>

#include "road_graph.hpp"
#include "thread_pool.hpp"

// EC2, R1 (reducción): las `limit` posiciones más cercanas en línea recta a un restaurante. Es el criterio de
// nearest() del despachador (simulation.cpp): distancia haversine desde donde termina el tramo actual de cada moto,
// empate por menor índice, y si hay `limit` o menos, todas en su orden. Acá trabaja sobre posiciones ya leídas
// (`at[p]` es la posición de la moto `ids[p]`), así se puede medir y comparar con la GPU sin las motos.
//
// Las tres funciones devuelven los ids elegidos en el mismo orden: de menor a mayor (distancia, id).

// Versión de un hilo: la de referencia (CPU, 1 hilo).
std::vector<std::size_t> nearestOf(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                   Coordinates shop, std::size_t limit);

// Versión multihilo (CPU, N hilos), como el paralelismo de datos de la carpeta 10: el pool reparte `at` en
// pool.size() tramos; cada trabajo calcula las distancias de su tramo y se queda con sus `limit` mejores (un
// combiner: el resultado privado de cada tramo); al final se combinan una sola vez. Da exactamente lo mismo que
// nearestOf: las claves (distancia, id) son todas distintas, así que las `limit` mejores de todas están entre las
// `limit` mejores de algún tramo.
std::vector<std::size_t> nearestOfParallel(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                           Coordinates shop, std::size_t limit, ThreadPool& pool);

// La selección exacta dentro de un subconjunto (`subset` = posiciones de `at`), como nearestOf. Si `subset`
// contiene a las `limit` mejores de todo `at` y tiene más de `limit` elementos, da lo mismo que nearestOf sobre
// todo `at`. La GPU devuelve ese subconjunto (ver gpu/nearest_gpu.hpp) y la CPU decide con esto.
std::vector<std::size_t> nearestWithin(const std::vector<Coordinates>& at, const std::vector<std::size_t>& ids,
                                       const std::vector<std::size_t>& subset, Coordinates shop, std::size_t limit);

// Cuánto se puede equivocar la distancia calculada en la GPU respecto de la de la CPU, con margen de sobra. Las
// dos usan la misma fórmula en double; sin, cos y asin de CUDA tienen un error de pocos ulp (unas 1e-16 relativas),
// o sea menos de 1e-11 m en distancias de pocos km. 1e-6 m deja un margen de cinco órdenes de magnitud y solo
// agrega al subconjunto motos que difieren en menos de 2 micrómetros.
constexpr double kNearestToleranceM = 1e-6;
