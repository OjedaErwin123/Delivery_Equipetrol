#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "thread_pool.hpp"

// EC2, P1 (performance: AoS contra SoA): las fases de cada pedido entregado y su promedio. Para cada pedido:
//   esperar     de creado a asignado;
//   recoger     de asignado a recogido (la cocina, la fila y la entrega en el puesto);
//   llevar      de recogido a entregado.
// Se suman las tres fases de todos los pedidos entregados (en 64 bits) y se cuentan. Enteros: la CPU y la GPU dan
// exactamente lo mismo.
//
// Los pedidos vienen como registros (AoS) de 32 bytes, como los arma quien relee el log: 8 campos, de los que el
// cálculo usa 4 (16 bytes). La versión SoA tiene un arreglo por campo y lee solo los 4 que usa.
struct OrderRecord {
    std::uint32_t createdMs = 0;
    std::uint32_t assignedMs = 0;
    std::uint32_t pickedUpMs = 0;
    std::uint32_t deliveredMs = 0;   // kNotYet si no se entregó
    std::uint32_t restaurant = 0;    // los otros cuatro campos no los usa este cálculo
    std::uint32_t courier = 0;
    std::uint32_t reassigned = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(OrderRecord) == 32, "un registro de 32 bytes");
constexpr std::uint32_t kNotYet = 0xffffffffu;

struct OrderColumns {  // SoA: los mismos datos, un arreglo por campo (solo los que usa el cálculo)
    std::vector<std::uint32_t> createdMs, assignedMs, pickedUpMs, deliveredMs;
};
OrderColumns columnsOf(const std::vector<OrderRecord>& records);

struct PhaseTotals {
    std::uint64_t delivered = 0;
    std::uint64_t waitMs = 0, pickupMs = 0, rideMs = 0;
    bool operator==(const PhaseTotals& o) const
    {
        return delivered == o.delivered && waitMs == o.waitMs && pickupMs == o.pickupMs && rideMs == o.rideMs;
    }
};

// CPU, 1 hilo, sobre los registros (AoS).
PhaseTotals phaseTotals(const std::vector<OrderRecord>& records);

// CPU, N hilos, sobre las columnas (SoA): tramos con sumas privadas, combinadas al final.
PhaseTotals phaseTotalsParallel(const OrderColumns& columns, ThreadPool& pool);
