// Punto de entrada provisional: demuestra el libro de pedidos con un productor y
// dos consumidores, y verifica que cada pedido sale exactamente una vez y en orden
// de llegada. Se reemplaza en el PR del flujo básico (generador, despachador, motos).
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
            if (!book.submit(std::move(order))) {  // aceptado: el pedido pasó al libro
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

    const bool ok = refused == 0 && exactlyOnce && fifo && book.size() == 0;
    std::cout << "rechazados por libro cerrado: " << refused << "\n"
              << "exactamente una vez: " << (exactlyOnce ? "OK" : "FALLA") << "\n"
              << "orden FIFO: " << (fifo ? "OK" : "FALLA") << "\n"
              << "libro vacío al final: " << (book.size() == 0 ? "OK" : "FALLA") << "\n";
    return ok ? 0 : 1;
}
