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
    // Qué pasó con un submit.
    enum class Submit { accepted, full, closed };

    // `capacity`: máximo de pedidos esperando adentro (maxPending); sin valor, sin límite
    // (las bandejas de las motos). Con 0 no acepta ninguno.
    explicit OrderBook(std::optional<std::size_t> capacity = std::nullopt) : _capacity(capacity) {}
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Agrega un pedido al final. Si el libro está lleno o cerrado lo dice y NO consume el
    // pedido (sigue en manos de quien llamó): un pedido nunca se pierde en silencio.
    // Mirar el tamaño y agregar es una sola operación con el lock tomado: comprobarlo
    // desde afuera con size() y después llamar a submit() sería check-then-act.
    [[nodiscard]] Submit submit(Order&& order);

    // Bloquea sin consumir CPU hasta que haya un pedido o el libro esté cerrado.
    // Devuelve std::nullopt solo cuando está cerrado y vacío: señal para dejar de consumir.
    std::optional<Order> take();

    // Como take(), pero sin esperar: un pedido si hay alguno, std::nullopt si no.
    std::optional<Order> tryTake();

    // No llegarán más pedidos. Despierta a TODOS los que esperan para que ninguno quede bloqueado.
    void close();

    // Cierre por detención: como close(), pero descarta lo que quede, así take() devuelve
    // std::nullopt de inmediato. Los pedidos descartados no terminaron: quedan pendientes.
    void abort();

    std::size_t size() const;

private:
    const std::optional<std::size_t> _capacity;
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<Order> _orders;
    bool _closed = false;
};
