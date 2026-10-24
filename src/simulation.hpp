#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "courier.hpp"
#include "event_log.hpp"
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

// Con cuántas motos estima el despachador cada pedido: las 8 con lugar más cercanas al restaurante.
// Decidido con el experimento de candidatos (README, DP.10): con 64 motos, estimar con todas hacía
// que cada ronda tardara más que lo que tarda en llegar el próximo pedido cuando estimar es caro
// (bajo TSan, 8,7 ms contra 2,0 ms): el libro se llenaba y se perdía un 26 % de las entregas.
constexpr std::size_t kNearestCandidates = 8;

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
    // Para los experimentos (DP.10); delivery_sim usa los valores por defecto.
    std::size_t poolThreads = 0;     // hilos del pool de estimaciones; 0 = hardware_concurrency() (2 si no informa)
    std::size_t candidateLimit = 0;  // con cuántas motos se estima cada pedido: las más cercanas; 0 = todas
                                     // (paramsFromConfig pone kNearestCandidates)
    std::int64_t meanIntervalMs = 36000;  // tiempo simulado medio entre dos pedidos (con ráfagas también)
    int burstMax = 1;                     // cada llegada trae entre 1 y burstMax pedidos en el mismo instante
    std::optional<std::size_t> maxPending;  // límite del libro (queueFull); sin valor, sin límite
    double breakdownProbability = 0;      // por viaje (con mapa); 0 = sin averías
    std::int64_t legMs = 60000;           // sin mapa: duración simulada de cada tramo de viaje
    double timeScale = 60.0;              // segundos simulados por segundo real
    std::uint32_t seed = 42;
    std::int64_t durationS = 0;           // fin por tiempo simulado; 0 = hasta una señal (o hasta `orders`)
    // Variante B: el bloque kitchen (pausas de la ventanilla). Sin valor, sin pausas.
    std::optional<KitchenConfig> pauses;
};

// Los parámetros que salen del archivo de configuración (ya validado): motos, semilla,
// intervalo medio, ráfagas, límite del libro, duración y escala de tiempo, los restaurantes,
// y como destinos todos los nodos que no son de un restaurante, con la tabla de alcanzabilidad
// (grafo vial: sentido único respetado). Sin tope de pedidos: se crean hasta la detención.
// Los tramos siguen siendo fijos (legMs) hasta que las motos se muevan por el grafo.
SimParams paramsFromConfig(const Config& config);

// Resumen final (sección 6.3). pending = created - delivered - rejected.
struct Summary {
    int created = 0;
    int delivered = 0;
    int rejected = 0;
    int breakdowns = 0;
    int reassigned = 0;  // pedidos traspasados (una línea orderReassigned cada uno)
    // Para los experimentos (no se imprimen): cuántos rechazos fueron noCourier y cuánto tardó (tiempo
    // REAL, ms) cada ronda de estimación, de encolar las tareas a tener los resultados (o el plazo).
    int noCourier = 0;
    std::vector<double> estimateRoundMs;
    std::int64_t avgDeliveryMs = 0;  // de la creación a la entrega
    std::int64_t maxDeliveryMs = 0;
    std::vector<std::pair<std::string, int>> deliveredPerCourier;
    // Variante B. hasPauses: la configuración trae el bloque kitchen.
    bool hasPauses = false;
    int pauses = 0;                         // líneas restaurantPaused
    std::int64_t pickupsDelayedByPause = 0;
};

// Una foto de la simulación en un frame, para dibujarla: cada moto (viaje y estado), los contadores
// y los pedidos activos tal como están en el log en ese momento, y la hora simulada del frame.
struct Frame {
    std::int64_t nowMs = 0;
    std::vector<CourierView> couriers;
    LiveStats stats;
};
// Se llama en cada frame (~30 por segundo) desde el hilo principal. false = se cerró la ventana:
// la simulación se detiene igual que con una señal.
using FrameHook = std::function<bool(const Frame&)>;

// Corre la simulación completa: imprime la línea de inicio en `out`, escribe los
// eventos en `logOut`, y vuelve recién cuando todos los hilos terminaron (join).
// Si se cumple `durationS`, registra simulationStopping, detiene todos los hilos y lo que
// no se entregó queda pendiente (pending = created - delivered en el resumen).
// Lo mismo si `stopSignal` se pone en true (lo pone el handler de SIGINT/SIGTERM), o si `onFrame`
// devuelve false.
Summary runSimulation(const SimParams& params, std::ostream& logOut, std::ostream& out,
                      const std::atomic<bool>& stopSignal, const FrameHook& onFrame = FrameHook());

// Sin señal: solo termina por tiempo o al entregar todos los pedidos (lo usan las pruebas).
Summary runSimulation(const SimParams& params, std::ostream& logOut, std::ostream& out);

// El resumen como una línea JSON, con el formato del enunciado. Con pausas de cocina (variante B),
// además "pauses" y "pickupsDelayedByPause" al final.
std::string toJson(const Summary& summary);
