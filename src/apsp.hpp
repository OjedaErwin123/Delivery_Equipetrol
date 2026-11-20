#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "road_graph.hpp"
#include "thread_pool.hpp"

// EC2, T1 (tiling): la tabla de distancias de TODOS contra TODOS por las calles (respetando el sentido único), en
// metros ENTEROS: cada calle redondeada al metro. Con enteros, el resultado de Floyd–Warshall es único y no
// depende del orden de las operaciones: la GPU, la CPU de un hilo y la de N hilos dan la misma tabla, bit a bit.
//
// En la simulación sirve para la alcanzabilidad de cada destino desde cada restaurante (unreachable): "hay camino"
// no depende del redondeo, así que sale idéntica a la de los Dijkstra de RoadGraph (apsp_check lo compara).
struct DistanceTable {
    // "No hay camino". Dos sumados todavía entran en 32 bits sin signo: la suma nunca desborda.
    static constexpr std::uint32_t kUnreachable = 0x3fffffffu;
    std::size_t n = 0;       // nodos de la red
    std::size_t stride = 0;  // largo de cada fila: n redondeado hacia arriba a múltiplo de kTile (para la GPU)
    std::vector<std::uint32_t> meters;  // fila por fila: meters[i * stride + j] = de i a j

    static constexpr std::size_t kTile = 32;
    std::uint32_t at(std::size_t from, std::size_t to) const { return meters[from * stride + to]; }
};

// La tabla inicial: 0 en la diagonal, el largo redondeado de cada calle (la más corta si hay dos entre los mismos
// nodos) y kUnreachable en el resto. Los nodos de relleno (de n a stride) quedan aislados.
DistanceTable initialTable(const RoadGraph& graph);

// La misma tabla inicial pero solo con los nodos `nodes` del grafo (y las calles entre ellos): para el barrido de
// tamaños del bench.
DistanceTable initialTable(const RoadGraph& graph, const std::vector<std::size_t>& nodes);

// CPU, 1 hilo: Floyd–Warshall clásico, k afuera; para cada k, d[i][j] = min(d[i][j], d[i][k] + d[k][j]).
void floydWarshall(DistanceTable& table);

// CPU, N hilos (la mejor versión de CPU): la misma idea de tiling que la GPU. Floyd–Warshall por bloques de 32 × 32
// (4 KB: tres bloques caben en la caché L1) en tres fases por bloque diagonal; los bloques de las fases 2 y 3 se
// reparten entre los hilos del pool (paralelismo de datos, carpeta 10) y se espera a todos antes de la fase
// siguiente. Por qué no el clásico con las filas repartidas: en cada uno de los n pasos recorre la tabla entera
// (con 4775 nodos, 92 MB por paso): queda limitado por la memoria y los hilos casi no ayudan (medido: 63 s con 1
// hilo, 61 s con 4). Por bloques, cada bloque cargado se reusa 32 veces desde la caché.
void floydWarshallBlocked(DistanceTable& table, ThreadPool& pool);

// reachable[r][d] como en paramsFromConfig: ¿hay camino del nodo restaurants[r] al nodo destinations[d]?
std::vector<std::vector<bool>> reachabilityFrom(const DistanceTable& table, const std::vector<std::size_t>& restaurants,
                                                const std::vector<std::size_t>& destinations);
