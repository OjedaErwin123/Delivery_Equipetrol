// Prueba del cupo de las mochilas (Fleet): reparto por turnos sin pasar bagCapacity, reserve
// duerme cuando todas están llenas y despierta al liberarse un cupo, abort despierta al que
// espera, y con un despachador y 4 motos en sus hilos nunca se pasa el cupo. Sale con 0 si todo está bien.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "fleet.hpp"
#include "log_line.hpp"
#include "order_book.hpp"

int main()
{
    bool ok = true;

    // 1. Tres motos con cupo 2: seis reservas por turnos, dos para cada una.
    {
        Fleet fleet(3, 2);
        std::vector<std::size_t> got;
        std::size_t next = 0;
        for (int k = 0; k < 6; ++k) {
            const std::size_t i = *fleet.reserve(next);
            got.push_back(i);
            next = (i + 1) % 3;
        }
        ok &= check("por turnos: 0 1 2 0 1 2", got == std::vector<std::size_t>{0, 1, 2, 0, 1, 2});
        ok &= check("cada una con 2 asignados (el cupo)",
                    fleet.assigned(0) == 2 && fleet.assigned(1) == 2 && fleet.assigned(2) == 2);

        // 2. Todas llenas: reserve duerme hasta que alguna libere un cupo, y le toca a esa.
        std::atomic<bool> done(false);
        std::optional<std::size_t> waited;
        std::thread dispatcher([&] {
            waited = fleet.reserve(0);
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // representa "un rato" (no coordina)
        const bool stillWaiting = !done;
        fleet.release(1);
        dispatcher.join();  // el join sincroniza: aquí ya se puede leer `waited`
        ok &= check("todas llenas: reserve espera (sin girar) y, al liberar la 1, se la da a la 1",
                    stillWaiting && waited == std::optional<std::size_t>(1));

        // 3. abort despierta al que espera.
        std::optional<std::size_t> aborted(7);
        std::thread blocked([&] { aborted = fleet.reserve(0); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fleet.abort();
        blocked.join();  // si abort no lo despertara, esto se colgaría
        ok &= check("abort: el que espera vuelve sin moto", !aborted.has_value());
    }

    // 4. Estrés: un despachador reserva 2000 veces y deja un "pedido" en la bandeja de la moto
    //    elegida; 4 motos (hilos) los sacan y liberan su cupo. Cada moto mira cuántos tiene
    //    asignados: nunca más que el cupo (3).
    {
        constexpr std::size_t kCouriers = 4, kCapacity = 3;
        constexpr int kOrders = 2000;
        Fleet fleet(kCouriers, kCapacity);
        std::vector<OrderBook> inboxes(kCouriers);
        std::atomic<bool> overCapacity(false);
        std::vector<std::thread> couriers;
        for (std::size_t i = 0; i < kCouriers; ++i) {
            couriers.emplace_back([&, i] {
                while (std::optional<Order> order = inboxes[i].take()) {
                    if (fleet.assigned(i) > kCapacity) {
                        overCapacity = true;
                    }
                    fleet.release(i);
                }
            });
        }
        std::size_t next = 0;
        for (int k = 0; k < kOrders; ++k) {
            const std::size_t i = *fleet.reserve(next);
            next = (i + 1) % kCouriers;
            if (inboxes[i].submit(Order("o" + std::to_string(k), "r0", "n0", 0)) != OrderBook::Submit::accepted) {
                overCapacity = true;
            }
        }
        for (OrderBook& inbox : inboxes) {
            inbox.close();
        }
        for (std::thread& t : couriers) {
            t.join();
        }
        bool allFree = true;
        for (std::size_t i = 0; i < kCouriers; ++i) {
            allFree = allFree && fleet.assigned(i) == 0;
        }
        ok &= check("estrés (2000 pedidos, 4 motos, cupo 3): nunca más que el cupo; al final todo liberado",
                    !overCapacity && allFree);
    }
    return ok ? 0 : 1;
}
