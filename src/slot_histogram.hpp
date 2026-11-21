#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "thread_pool.hpp"

// EC2, H2 (histograma 2D): los pedidos creados por (restaurante, franja de 10 minutos simulados). La caja
// [r][s] cuenta los pedidos del restaurante r creados en [s·10 min, (s+1)·10 min); la última franja (143) junta
// todo lo de 23 h 50 min en adelante. Sirve para ver las horas pico de cada restaurante. Conteos enteros: la CPU de
// un hilo, la de N hilos y la GPU dan exactamente lo mismo.
constexpr std::size_t kSlots = 144;          // 24 h de franjas de 10 min
constexpr std::uint32_t kSlotMs = 600000;    // 10 min simulados
constexpr std::size_t kMaxSlotRestaurants = 64;  // 64 x 144 cajas de 4 bytes = 36 KB: caben en memoria compartida

inline std::size_t slotOf(std::uint32_t createdMs)
{
    const std::size_t s = createdMs / kSlotMs;
    return s < kSlots ? s : kSlots - 1;
}

using SlotHistogram = std::vector<std::uint64_t>;  // restaurants * kSlots cajas, fila por restaurante

// CPU, 1 hilo.
SlotHistogram ordersPerSlot(const std::vector<std::uint32_t>& restaurantOf, const std::vector<std::uint32_t>& createdMs,
                            std::size_t restaurants);

// CPU, N hilos: histogramas privados por tramo, sumados una vez al final (combiners, carpeta 10).
SlotHistogram ordersPerSlotParallel(const std::vector<std::uint32_t>& restaurantOf,
                                    const std::vector<std::uint32_t>& createdMs, std::size_t restaurants,
                                    ThreadPool& pool);
