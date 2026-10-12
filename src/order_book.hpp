#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "order.hpp"

// Libro de pedidos: un monitor (como 07/example_42).
//
// Los datos (_orders, _closed) son privados y solo se tocan dentro de métodos que
// toman _mutex, así que ningún hilo puede leerlos ni modificarlos sin el lock:
// no hay data race posible sobre ellos.
//
// Diferencias con example_42:
//   - FIFO (deque + pop_front): los pedidos salen en orden de llegada. El ejemplo
//     usa vector + pop_back, que es LIFO.
//   - submit() no duerme: nunca se pausa con el lock tomado.
//   - notify después de soltar el lock: el hilo despertado no choca con un mutex aún tomado.
class OrderBook {
public:
    OrderBook() = default;
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Agrega un pedido al final. Si el libro ya está cerrado devuelve false y NO
    // consume el pedido (sigue en manos de quien llamó): un pedido nunca se pierde en silencio.
    [[nodiscard]] bool submit(Order&& order);

    // Bloquea sin consumir CPU hasta que haya un pedido o el libro esté cerrado.
    // Devuelve std::nullopt solo cuando está cerrado y vacío: señal para dejar de consumir.
    std::optional<Order> take();

    // No llegarán más pedidos. Despierta a TODOS los que esperan para que ninguno quede bloqueado.
    void close();

    std::size_t size() const;

private:
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<Order> _orders;
    bool _closed = false;
};
