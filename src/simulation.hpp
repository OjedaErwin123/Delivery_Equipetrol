#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// Parámetros de la simulación. En esta versión van fijos en el código; más adelante
// salen del archivo de configuración. Con estos valores la corrida simula ~1,5 h
// (~90 s reales): 4 motos atienden 1 pedido cada 30 s simulados y llega 1 cada 36 s.
struct SimParams {
    int couriers = 4;
    int orders = 150;
    int restaurants = 7;
    int nodes = 154;
    std::int64_t meanIntervalMs = 36000;  // tiempo simulado medio entre pedidos
    std::int64_t legMs = 60000;           // duración simulada de cada tramo de viaje
    double timeScale = 60.0;              // segundos simulados por segundo real
    std::uint32_t seed = 42;
    std::int64_t durationS = 0;           // fin por tiempo simulado; 0 = sin límite (termina al entregar `orders`)
};

// Resumen final (sección 6.3). Los campos que esta versión todavía no produce
// (rechazados, reasignados, averías) se imprimen en 0.
struct Summary {
    int created = 0;
    int delivered = 0;
    std::int64_t avgDeliveryMs = 0;  // de la creación a la entrega
    std::int64_t maxDeliveryMs = 0;
    std::vector<std::pair<std::string, int>> deliveredPerCourier;
};

// Corre la simulación completa: imprime la línea de inicio en `out`, escribe los
// eventos en `logOut`, y vuelve recién cuando todos los hilos terminaron (join).
// Si se cumple `durationS`, registra simulationStopping, detiene todos los hilos y lo que
// no se entregó queda pendiente (pending = created - delivered en el resumen).
Summary runSimulation(const SimParams& params, std::ostream& logOut, std::ostream& out);

// El resumen como una línea JSON, con el formato del enunciado.
std::string toJson(const Summary& summary);
