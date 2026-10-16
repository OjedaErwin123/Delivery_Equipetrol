// Prueba del libro de pedidos con un productor y dos consumidores: verifica que cada
// pedido sale exactamente una vez y en orden de llegada. Sale con 0 si todo está bien.
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "order_book.hpp"

namespace {

constexpr int kOrders = 1000;
constexpr int kConsumers = 2;

int orderNumber(const std::string& id) { return std::stoi(id.substr(1)); }  // "o17" -> 17

}  // namespace

int main()
{
    // El libro vive en la pila de main y todos los hilos terminan (join) antes de
    // que se destruya: compartirlo por referencia es seguro aquí.
    OrderBook book;

    // Cada consumidor anota lo que toma en SU propio vector: no se comparte nada
    // entre consumidores, así que no hace falta lock. main los lee recién después
    // de los join, que establecen el happens-before.
    std::vector<std::vector<int>> taken(kConsumers);
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&book, &mine = taken[c]] {
            while (std::optional<Order> order = book.take()) {
                mine.push_back(orderNumber(order->id()));
            }
        });
    }

    // Solo el productor escribe 'refused'; main lo lee después del join.
    int refused = 0;
    std::thread producer([&book, &refused] {
        for (int i = 0; i < kOrders; ++i) {
            Order order("o" + std::to_string(i), "r" + std::to_string(i % 6), "n" + std::to_string(i % 25), i);
            // Si se acepta, el pedido pasa al libro y 'order' queda vacío; si no, sigue aquí.
            if (book.submit(std::move(order)) != OrderBook::Submit::accepted) {
                ++refused;
            }
        }
        book.close();
    });

    producer.join();
    for (std::thread& consumer : consumers) {
        consumer.join();
    }

    // Verificación 1: cada consumidor recibió sus pedidos en orden creciente (FIFO).
    bool fifo = true;
    std::vector<int> all;
    for (int c = 0; c < kConsumers; ++c) {
        fifo = fifo && std::is_sorted(taken[c].begin(), taken[c].end());
        all.insert(all.end(), taken[c].begin(), taken[c].end());
        std::cout << "consumidor " << c << ": " << taken[c].size() << " pedidos\n";
    }

    // Verificación 2: entre todos salieron o0..o999, cada uno exactamente una vez.
    std::sort(all.begin(), all.end());
    bool exactlyOnce = all.size() == static_cast<std::size_t>(kOrders);
    for (std::size_t i = 0; exactlyOnce && i < all.size(); ++i) {
        exactlyOnce = all[i] == static_cast<int>(i);
    }

    bool ok = refused == 0 && exactlyOnce && fifo && book.size() == 0;
    std::cout << "rechazados por libro cerrado: " << refused << "\n"
              << "exactamente una vez: " << (exactlyOnce ? "OK" : "FALLA") << "\n"
              << "orden FIFO: " << (fifo ? "OK" : "FALLA") << "\n"
              << "libro vacío al final: " << (book.size() == 0 ? "OK" : "FALLA") << "\n";

    // Verificación 3: con límite (maxPending), lleno rechaza sin consumir el pedido; al sacar
    // uno vuelve a aceptar; con 0 no acepta nada; cerrado gana a lleno.
    OrderBook limited(2);
    Order a("a", "r0", "n0", 0), b("b", "r0", "n0", 0), c("c", "r0", "n0", 0);
    const bool firstTwo = limited.submit(std::move(a)) == OrderBook::Submit::accepted &&
                          limited.submit(std::move(b)) == OrderBook::Submit::accepted;
    const bool thirdFull = limited.submit(std::move(c)) == OrderBook::Submit::full;
    const bool notConsumed = c.id() == "c";  // submit no lo movió: sigue en manos de quien llamó
    const bool takenFirst = limited.take()->id() == "a";
    const bool acceptsAgain = limited.submit(std::move(c)) == OrderBook::Submit::accepted && limited.size() == 2;
    OrderBook zero(0);
    Order d("d", "r0", "n0", 0);
    const bool zeroFull = zero.submit(std::move(d)) == OrderBook::Submit::full && zero.size() == 0;
    limited.close();
    Order e("e", "r0", "n0", 0);
    const bool closedWins = limited.submit(std::move(e)) == OrderBook::Submit::closed;
    const bool capacityOk = firstTwo && thirdFull && notConsumed && takenFirst && acceptsAgain && zeroFull && closedWins;
    std::cout << "límite: lleno rechaza sin consumir, vuelve a aceptar, 0 no acepta nada, cerrado gana: "
              << (capacityOk ? "OK" : "FALLA") << "\n";
    ok = ok && capacityOk;
    return ok ? 0 : 1;
}
