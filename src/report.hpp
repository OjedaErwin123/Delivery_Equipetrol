#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "simulation.hpp"

// EC2: el reporte de la corrida (`delivery_sim ... --report <archivo.json>`). Al terminar la simulación (después del
// resumen, que sigue siendo la última línea de stdout), delivery_sim vuelve a leer el log que escribió (una
// "repetición del log") y calcula las aplicaciones de GPU de tipo A: análisis nuevos que NO cambian ninguna decisión
// de la simulación. Con USE_CUDA se calculan en la GPU (por el hilo dueño); sin CUDA, en la CPU. El reporte dice
// dónde se calculó cada parte.

// Lo que el reporte necesita del log, leído una vez.
struct LogReplay {
    std::vector<std::uint32_t> deliveryMs;  // por pedido entregado: t(orderDelivered) - t(orderCreated)
    std::uint64_t delivered = 0;            // líneas orderDelivered
    // SC1: los restaurantes en el orden en que aparecen por primera vez en el log, y por pedido creado (en el orden
    // de las líneas orderCreated) el índice de su restaurante en `restaurants`.
    std::vector<std::string> restaurants;
    std::vector<std::uint32_t> restaurantOf;
};

// Lee un log de delivery_sim (un JSON por línea). Lanza std::runtime_error con el número de línea si una no se
// puede leer.
LogReplay replayLog(std::istream& log);

// Escribe el reporte en JSON. `gpu` puede ser nullptr (todo en la CPU).
void writeReport(const LogReplay& replay, GpuOwner* gpu, std::ostream& out);
