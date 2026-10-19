#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "travel.hpp"

// Pasar la comida de una moto averiada a la que la viene a buscar (15 s simulados, en el punto de la avería).
constexpr std::int64_t kRescueMs = 15000;

// Un pedido asignado, como lo ve quien estima: de dónde sale, a dónde va, cuándo está la comida y
// si ya lo recogieron (y, si lo heredó de una moto averiada, dónde quedó la comida). Es una COPIA
// de los datos del pedido (el pedido mismo es move-only y vive en un solo lugar).
struct PlannedOrder {
    PlannedOrder(std::string fromRestaurant, std::string to, std::int64_t readyAt, bool isPicked,
                 std::optional<Place> foodAt = std::nullopt)
        : restaurant(std::move(fromRestaurant)), destination(std::move(to)), readyAtMs(readyAt), picked(isPicked),
          fetchAt(std::move(foodAt))
    {
    }

    std::string restaurant;
    std::string destination;
    std::int64_t readyAtMs = 0;
    bool picked = false;
    std::optional<Place> fetchAt;  // recogido, pero la comida está en el punto de una avería
};

// Lo que una moto tiene por delante: su viaje actual (o quieta) y sus pedidos asignados en orden
// de asignación (FIFO). La moto publica su plan con su mutex y entrega copias (05: copiar evita
// compartir): las estimaciones trabajan sobre la copia, en paralelo, sin tener ningún lock.
struct CourierPlan {
    explicit CourierPlan(Trip current) : trip(std::move(current)) {}
    Trip trip;
    std::vector<PlannedOrder> orders;
};

// Una estimación que no se pudo calcular (p. ej. la moto no tiene camino al restaurante). Viaja
// por el future de la tarea hasta el despachador, que la reporta y descarta a ese candidato.
class EstimateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// El ETA: el instante simulado en que se entregaría `order` si se le asignara ahora (`nowMs`) a la
// moto con este `plan`. Simula su recorrido: parte del final del tramo actual (no gira a mitad de
// una calle), termina lo que ya lleva en orden FIFO, recoge en cada visita todos los pedidos de ese
// restaurante (esperando la comida del último y la entrega de kHandoverMs), va a buscar la comida
// heredada al punto de una avería (kRescueMs), y lleva `order` al cliente. No incluye la fila del restaurante (no se puede saber de antemano).
// Función pura: no toca estado compartido. Lanza EstimateError si algún tramo no tiene camino.
std::int64_t estimateDelivery(const World& world, const CourierPlan& plan, const PlannedOrder& order,
                              std::int64_t nowMs);
