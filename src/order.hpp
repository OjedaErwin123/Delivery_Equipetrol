#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

// Un pedido de la simulación.
//
// Es move-only: copiarlo crearía un segundo ejemplar del mismo pedido (dos motos
// podrían "entregar" el mismo o17), así que la copia está borrada y el compilador
// rechaza cualquier intento. El pedido pasa de un dueño a otro solo con std::move
// y existe en un único lugar a la vez: el libro, el despachador o la bolsa de una moto.
class Order {
public:
    // `prepMs`: cuánto tarda la cocina; empieza a contar cuando se asigna el pedido.
    Order(std::string id, std::string restaurant, std::string destination, std::int64_t createdAtMs,
          std::int64_t prepMs = 0)
        : _id(std::move(id)),
          _restaurant(std::move(restaurant)),
          _destination(std::move(destination)),
          _createdAtMs(createdAtMs),
          _prepMs(prepMs)
    {
    }

    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
    Order(Order&&) = default;
    Order& operator=(Order&&) = default;
    ~Order() = default;

    const std::string& id() const { return _id; }
    const std::string& restaurant() const { return _restaurant; }
    const std::string& destination() const { return _destination; }
    std::int64_t createdAtMs() const { return _createdAtMs; }
    std::int64_t prepMs() const { return _prepMs; }

    // Lo marca el despachador al asignarlo: ahí empieza la preparación (no se cocina nada rechazado).
    void markAssigned(std::int64_t t) { _assignedAtMs = t; }
    // Cuándo está lista la comida: asignado + preparación.
    std::int64_t readyAtMs() const { return _assignedAtMs + _prepMs; }

private:
    std::string _id;
    std::string _restaurant;   // id del restaurante (p. ej. "r3")
    std::string _destination;  // id del nodo de entrega (p. ej. "n42")
    std::int64_t _createdAtMs; // tiempo simulado de creación
    std::int64_t _prepMs;
    std::int64_t _assignedAtMs = 0;
};

static_assert(!std::is_copy_constructible_v<Order>, "un pedido no se copia: existe en un solo lugar");
// Los contenedores mueven (en vez de copiar) solo si el move no lanza; sin copia,
// un move que pudiera lanzar dejaría a std::deque sin forma segura de reubicar pedidos.
static_assert(std::is_nothrow_move_constructible_v<Order>, "el move de Order debe ser noexcept");
