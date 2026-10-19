#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "travel.hpp"

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
    // reachable[r][d]: ¿hay camino por las calles del restaurante r al destino d? Vacío = todo
    // alcanzable (las pruebas sin mapa). Se arma antes de lanzar hilos y nadie lo modifica después.
    std::vector<std::vector<bool>> reachable;
    // El mapa (grafo, restaurantes, inicio, velocidad), compartido por todas las motos y de solo
    // lectura. nullptr = sin mapa (las pruebas): cada tramo dura legMs.
    std::shared_ptr<const World> world;
    // Por restaurante (en el orden de `restaurants`): puestos y rango de preparación. Vacío = sin
    // restaurantes con puestos (las pruebas): la recogida es instantánea y no hay preparación.
    struct Kitchen {
        std::size_t pickupSlots = 1;
        std::int64_t prepMinMs = 0, prepMaxMs = 0;
    };
    std::vector<Kitchen> kitchens;
    // Máximo de pedidos asignados (y sin entregar) por moto; sin valor = sin límite (pruebas).
    std::optional<std::size_t> bagCapacity;
    // Con mapa, el despachador estima: espera las estimaciones hasta quoteTimeoutMs (tiempo REAL) y
    // rechaza noCourier un pedido sin asignar a los acceptTimeoutMs (tiempo SIMULADO) de creado.
    std::int64_t quoteTimeoutMs = 200;
    std::int64_t acceptTimeoutMs = 600000;
    std::int64_t meanIntervalMs = 36000;  // tiempo simulado medio entre dos pedidos (con ráfagas también)
    int burstMax = 1;                     // cada llegada trae entre 1 y burstMax pedidos en el mismo instante
    std::optional<std::size_t> maxPending;  // límite del libro (queueFull); sin valor, sin límite
    double breakdownProbability = 0;      // por viaje (con mapa); 0 = sin averías
    std::int64_t legMs = 60000;           // sin mapa: duración simulada de cada tramo de viaje
    double timeScale = 60.0;              // segundos simulados por segundo real
    std::uint32_t seed = 42;
    std::int64_t durationS = 0;           // fin por tiempo simulado; 0 = hasta una señal (o hasta `orders`)
};

// Los parámetros que salen del archivo de configuración (ya validado): motos, semilla,
// intervalo medio, ráfagas, límite del libro, duración y escala de tiempo, los restaurantes,
// y como destinos todos los nodos que no son de un restaurante, con la tabla de alcanzabilidad
// (grafo vial: sentido único respetado). Sin tope de pedidos: se crean hasta la detención.
// Los tramos siguen siendo fijos (legMs) hasta que las motos se muevan por el grafo.
SimParams paramsFromConfig(const Config& config);

// Resumen final (sección 6.3). Los reasignados todavía no se producen: se imprimen en 0.
// pending = created - delivered - rejected.
struct Summary {
    int created = 0;
    int delivered = 0;
    int rejected = 0;
    int breakdowns = 0;
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
