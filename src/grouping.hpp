#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "thread_pool.hpp"

// EC2, SC1 (scan): los pedidos agrupados por restaurante con un counting sort ESTABLE. Entrada: el restaurante de
// cada pedido (en el orden en que se crearon), un índice en [0, restaurants). Salida:
//   offsets[r]  dónde empieza el grupo del restaurante r (offsets[restaurants] = cantidad de pedidos): el scan
//               exclusivo de los conteos por restaurante;
//   order       los pedidos (sus índices) agrupados por restaurante; dentro de cada grupo, en el orden de creación.
// Es lo que hace std::stable_sort por restaurante, en tiempo lineal. Enteros y orden estable: la CPU de un hilo, la
// de N hilos y la GPU dan exactamente lo mismo.
struct Grouping {
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> order;
    bool operator==(const Grouping& other) const { return offsets == other.offsets && order == other.order; }
};

// CPU, 1 hilo: contar, scan exclusivo, repartir en orden.
Grouping groupByRestaurant(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants);

// CPU, N hilos (counting sort paralelo, carpeta 10): cada trabajo del pool cuenta su tramo contiguo en sus propios
// contadores; un scan exclusivo sobre la tabla (restaurante, tramo), en ese orden, da dónde escribe cada tramo cada
// restaurante; y cada trabajo reparte su tramo en orden. Estable: los tramos son contiguos y están en orden.
Grouping groupByRestaurantParallel(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants,
                                   ThreadPool& pool);
