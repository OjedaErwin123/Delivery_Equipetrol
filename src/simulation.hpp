#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"

// Ids "r0", "r1", ... (o "n0", ...): los valores por defecto de las pruebas.
inline std::vector<std::string> numberedIds(const std::string& prefix, int count)
{
    std::vector<std::string> ids;
    for (int i = 0; i < count; ++i) {
        ids.push_back(prefix + std::to_string(i));
    }
    return ids;
}

// Parámetros de la simulación. delivery_sim los arma desde el archivo de configuración
// (paramsFromConfig); las pruebas usan estos valores por defecto y cambian lo que necesitan.
struct SimParams {
    int couriers = 4;
    int orders = 0;                       // tope de pedidos creados; 0 = sin tope (hasta la detención)
    std::vector<std::string> restaurants = numberedIds("r", 7);  // de dónde salen los pedidos
    std::vector<std::string> destinations = numberedIds("n", 154);  // puntos de entrega posibles
    std::int64_t meanIntervalMs = 36000;  // tiempo simulado medio entre dos pedidos (con ráfagas también)
    int burstMax = 1;                     // cada llegada trae entre 1 y burstMax pedidos en el mismo instante
    std::optional<std::size_t> maxPending;  // límite del libro (queueFull); sin valor, sin límite
    std::int64_t legMs = 60000;           // duración simulada de cada tramo de viaje (v0: fija)
    double timeScale = 60.0;              // segundos simulados por segundo real
    std::uint32_t seed = 42;
    std::int64_t durationS = 0;           // fin por tiempo simulado; 0 = hasta una señal (o hasta `orders`)
};

// Los parámetros que salen del archivo de configuración (ya validado): motos, semilla,
// intervalo medio, ráfagas, límite del libro, duración y escala de tiempo, los restaurantes,
// y como destinos todos los nodos que no son de un restaurante. Sin tope de pedidos: se crean hasta la detención.
// Los tramos siguen siendo fijos (legMs) hasta que las motos se muevan por el grafo.
SimParams paramsFromConfig(const Config& config);

// Resumen final (sección 6.3). Los campos que esta versión todavía no produce
// (reasignados, averías) se imprimen en 0. pending = created - delivered - rejected.
struct Summary {
    int created = 0;
    int delivered = 0;
    int rejected = 0;
    std::int64_t avgDeliveryMs = 0;  // de la creación a la entrega
    std::int64_t maxDeliveryMs = 0;
    std::vector<std::pair<std::string, int>> deliveredPerCourier;
};

// Corre la simulación completa: imprime la línea de inicio en `out`, escribe los
// eventos en `logOut`, y vuelve recién cuando todos los hilos terminaron (join).
// Si se cumple `durationS`, registra simulationStopping, detiene todos los hilos y lo que
// no se entregó queda pendiente (pending = created - delivered en el resumen).
// Lo mismo si `stopSignal` se pone en true (lo pone el handler de SIGINT/SIGTERM).
Summary runSimulation(const SimParams& params, std::ostream& logOut, std::ostream& out,
                      const std::atomic<bool>& stopSignal);

// Sin señal: solo termina por tiempo o al entregar todos los pedidos (lo usan las pruebas).
Summary runSimulation(const SimParams& params, std::ostream& logOut, std::ostream& out);

// El resumen como una línea JSON, con el formato del enunciado.
std::string toJson(const Summary& summary);
