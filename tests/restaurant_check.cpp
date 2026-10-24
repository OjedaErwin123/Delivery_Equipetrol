// Prueba del restaurante: nunca más motos recogiendo que puestos, atención en orden de
// llegada, abort despierta a los que hacen fila y el puesto se libera solo. Con pausas de cocina:
// nadie toma un puesto durante una pausa, la fila se atiende al terminar en el mismo orden, y las
// demoras se cuentan bien. Sale con 0 si todo está bien.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "log_line.hpp"
#include "restaurant.hpp"
#include "sim_clock.hpp"

namespace {

// Representa el tiempo de entrega de la comida en el puesto (no coordina hilos).
void handover() { std::this_thread::sleep_for(std::chrono::microseconds(200)); }

}  // namespace

int main()
{
    bool ok = true;
    const SimClock clock(1000.0);  // 1 ms real = 1 s simulado: las horas distinguen llegadas cercanas

    // 1. Un puesto, 16 motos que llegan a la vez: entran de a una y en orden de ticket
    //    (el ticket es el orden en que llegaron al restaurante).
    {
        Restaurant r("r0", 1);
        std::mutex m;  // de la prueba: protege `served`
        std::vector<std::uint64_t> served;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 16; ++i) {
            couriers.emplace_back([&r, &m, &served, &clock] {
                std::optional<Restaurant::Slot> slot = r.enter(clock);
                {
                    std::lock_guard<std::mutex> lock(m);
                    served.push_back(slot->ticket());  // con el puesto tomado: nadie más puede estar aquí
                }
                handover();
            });
        }
        for (std::thread& t : couriers) {
            t.join();
        }
        bool inOrder = served.size() == 16;
        for (std::size_t i = 0; inOrder && i < served.size(); ++i) {
            inOrder = served[i] == i;
        }
        ok &= check("1 puesto: atendidos en orden de llegada (tickets 0, 1, 2, ...)", inOrder);
        ok &= check("1 puesto: al final no queda ninguno ocupado", r.inUse() == 0);
    }

    // 2. Tres puestos, 24 motos que vuelven 20 veces cada una: nunca más de 3 adentro.
    {
        Restaurant r("r1", 3);
        std::mutex m;
        int inside = 0;
        int maxInside = 0;
        int served = 0;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 24; ++i) {
            couriers.emplace_back([&] {
                for (int k = 0; k < 20; ++k) {
                    std::optional<Restaurant::Slot> slot = r.enter(clock);
                    {
                        std::lock_guard<std::mutex> lock(m);
                        ++inside;
                        maxInside = std::max(maxInside, inside);
                        ++served;
                    }
                    handover();
                    {
                        std::lock_guard<std::mutex> lock(m);
                        --inside;
                    }
                }  // el Slot se destruye aquí: libera el puesto
            });
        }
        for (std::thread& t : couriers) {
            t.join();
        }
        std::cout << "3 puestos: máximo de motos recogiendo a la vez = " << maxInside << "\n";
        ok &= check("3 puestos: nunca más de 3 a la vez", maxInside >= 1 && maxInside <= 3);
        ok &= check("3 puestos: todos atendidos (24 x 20)", served == 24 * 20);
        ok &= check("3 puestos: al final no queda ninguno ocupado", r.inUse() == 0);
    }

    // 3. Detención: con el único puesto tomado, las motos que hacen fila despiertan con
    //    std::nullopt al abortar, y las que llegan después no esperan.
    {
        Restaurant r("r2", 1);
        std::optional<Restaurant::Slot> held = r.enter(clock);
        std::mutex m;
        int refused = 0;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 5; ++i) {
            couriers.emplace_back([&r, &m, &refused, &clock] {
                const bool got = r.enter(clock).has_value();
                std::lock_guard<std::mutex> lock(m);
                refused += got ? 0 : 1;
            });
        }
        r.abort();
        for (std::thread& t : couriers) {
            t.join();  // si abort no despertara a todos, esto se colgaría
        }
        ok &= check("abort: las 5 motos en fila vuelven sin puesto", refused == 5);
        ok &= check("abort: una moto que llega después no espera", !r.enter(clock));
        held.reset();
        ok &= check("abort: el puesto que estaba tomado se libera igual", r.inUse() == 0);
    }

    // 4. El puesto es move-only y se libera una sola vez, al destruirse su dueño final.
    {
        Restaurant r("r3", 2);
        std::optional<Restaurant::Slot> a = r.enter(clock);
        Restaurant::Slot moved(std::move(*a));
        a.reset();  // el Slot vacío (movido) no libera nada
        ok &= check("move: sigue ocupado tras mover y destruir el original", r.inUse() == 1);
        {
            Restaurant::Slot last(std::move(moved));
        }
        ok &= check("move: se libera al destruirse el último dueño", r.inUse() == 0);
    }

    // 5. Las horas que lleva el Slot (las que van al log como arrivedAt y t de pickupStarted): con
    //    2 puestos y 32 motos que llegan a la vez, quien llegó antes nunca toma el puesto después
    //    (lo que un programa puede verificar mirando solo el log).
    {
        Restaurant r("r4", 2);
        std::mutex m;
        std::vector<std::pair<std::int64_t, std::int64_t>> times;  // (llegada, puesto)
        std::vector<std::thread> couriers;
        for (int i = 0; i < 32; ++i) {
            couriers.emplace_back([&] {
                std::optional<Restaurant::Slot> slot = r.enter(clock);
                {
                    std::lock_guard<std::mutex> lock(m);
                    times.emplace_back(slot->arrivedAtMs(), slot->startedAtMs());
                }
                handover();
            });
        }
        for (std::thread& t : couriers) {
            t.join();
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> byArrival = times;
        std::sort(byArrival.begin(), byArrival.end());
        bool fifo = byArrival.size() == 32, waited = true;
        for (std::size_t i = 0; i < byArrival.size(); ++i) {
            waited = waited && byArrival[i].first <= byArrival[i].second;
            fifo = fifo && (i == 0 || byArrival[i - 1].second <= byArrival[i].second);
        }
        ok &= check("horas: nadie toma el puesto antes de llegar", waited);
        ok &= check("horas: quien llegó antes no toma el puesto después (FIFO visible en el log)", fifo);
    }

    // 6. El calendario de pausas: escalonado con división entera, y las cuentas en los bordes.
    {
        const PauseSchedule r1 = PauseSchedule::forRestaurant(900000, 240000, 1, 7);
        ok &= check("calendario: el restaurante 1 de 7 empieza en 900000/7 = 128571", r1.offset == 128571);
        ok &= check("calendario: [inicio, fin) cerrado a la izquierda, abierto a la derecha",
                    !r1.pausedAt(128570) && r1.pausedAt(128571) && r1.pausedAt(368570) && !r1.pausedAt(368571) &&
                        r1.pausedAt(900000 + 128571) && r1.pauseEnd(200000) == 368571);
        ok &= check("calendario: pausas que empiezan en (desde, hasta)",
                    r1.startsBetween(0, 128571) == 0 && r1.startsBetween(0, 128572) == 1 &&
                        r1.startsBetween(128571, 2000000) == 2 && r1.startsBetween(100000, 1100000) == 2);
        const PauseSchedule none = PauseSchedule::forRestaurant(900000, 0, 0, 7);
        ok &= check("calendario: pauseMs 0 no tiene pausas", !none.pausedAt(0) && none.startsBetween(0, 5000000) == 0);
    }

    // 7. Una pausa con fila: 1 puesto, pausa en [100 s, 1100 s) simulados (100 ms a 1,1 s reales).
    //    A toma el puesto antes de la pausa y lo tiene 300 ms; E llega mientras A lo tiene (ya está en
    //    la fila cuando empieza la pausa); B, C y D llegan durante la pausa. Nadie toma el puesto dentro
    //    de la pausa; E, B, C y D entran después, en orden de llegada, de a uno; y hay 4 demoras.
    {
        const SimClock clock(1000.0);  // propio: t = 0 al empezar este caso (los anteriores ya consumieron tiempo)
        const PauseSchedule pause{10000000, 1000000, 100000};
        Restaurant r("r5", 1, pause);
        std::mutex m;
        int inside = 0, maxInside = 0;
        std::vector<std::pair<std::int64_t, std::int64_t>> times;  // (llegada, puesto), de las 5 motos
        const auto visit = [&](std::chrono::milliseconds hold) {
            std::optional<Restaurant::Slot> slot = r.enter(clock);
            {
                std::lock_guard<std::mutex> lock(m);
                times.emplace_back(slot->arrivedAtMs(), slot->startedAtMs());
                maxInside = std::max(maxInside, ++inside);
            }
            std::this_thread::sleep_for(hold);  // representa la entrega en el puesto (no coordina)
            std::lock_guard<std::mutex> lock(m);
            --inside;
        };
        // Los sleep_for de abajo representan "llega en tal momento" (no coordinan hilos).
        std::vector<std::thread> couriers;
        couriers.emplace_back(visit, std::chrono::milliseconds(300));  // A
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        couriers.emplace_back(visit, std::chrono::milliseconds(50));  // E
        std::this_thread::sleep_for(std::chrono::milliseconds(380));
        for (int i = 0; i < 3; ++i) {  // B, C, D
            couriers.emplace_back(visit, std::chrono::milliseconds(50));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        for (std::thread& t : couriers) {
            t.join();
        }
        std::sort(times.begin(), times.end());  // por llegada
        bool outside = times.size() == 5, fifo = true, afterPause = true;
        for (std::size_t i = 0; i < times.size(); ++i) {
            outside = outside && !pause.pausedAt(times[i].second);
            fifo = fifo && (i == 0 || times[i - 1].second <= times[i].second);
            afterPause = afterPause && (i == 0 || times[i].second >= 1100000);
        }
        std::cout << "pausa: llegadas/puestos (s simulados):";
        for (const auto& t : times) {
            std::cout << " " << t.first / 1000 << "/" << t.second / 1000;
        }
        std::cout << "; demoras = " << r.delayedByPause() << "\n";
        ok &= check("pausa: nadie toma un puesto dentro de la pausa", outside);
        ok &= check("pausa: los que esperaban entran después del fin, en orden de llegada", afterPause && fifo);
        ok &= check("pausa: pickupSlots se respeta (1 a la vez)", maxInside == 1);
        ok &= check("pausa: 4 demoras (E ya estaba en la fila; B, C y D entraron en pausa)", r.delayedByPause() == 4);
    }

    // 8. Detención en mitad de una pausa: las motos en la fila despiertan enseguida, sin puesto.
    {
        Restaurant r("r6", 1, PauseSchedule{100000000, 99000000, 0});  // en pausa desde t = 0, por ~27 h simuladas
        std::mutex m;
        int refused = 0;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 3; ++i) {
            couriers.emplace_back([&] {
                const bool got = r.enter(clock).has_value();
                std::lock_guard<std::mutex> lock(m);
                refused += got ? 0 : 1;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // representa "en algún momento"
        const auto stopAt = std::chrono::steady_clock::now();
        r.abort();
        for (std::thread& t : couriers) {
            t.join();  // si la espera con plazo no despertara con abort, esto tardaría horas
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopAt);
        std::cout << "pausa + abort: " << refused << " sin puesto, en " << ms.count() << " ms\n";
        ok &= check("pausa + abort: las motos en la fila despiertan enseguida, sin puesto",
                    refused == 3 && ms.count() < 1000);
    }

    return ok ? 0 : 1;
}
