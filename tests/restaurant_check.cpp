// Prueba del restaurante: nunca más motos recogiendo que puestos, atención en orden de
// llegada, abort despierta a los que hacen fila y el puesto se libera solo. Sale con 0 si todo está bien.
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

namespace {

// Representa el tiempo de entrega de la comida en el puesto (no coordina hilos).
void handover() { std::this_thread::sleep_for(std::chrono::microseconds(200)); }

}  // namespace

int main()
{
    bool ok = true;

    // 1. Un puesto, 16 motos que llegan a la vez: entran de a una y en orden de ticket
    //    (el ticket es el orden en que llegaron al restaurante).
    {
        Restaurant r("r0", 1);
        std::mutex m;  // de la prueba: protege `served`
        std::vector<std::uint64_t> served;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 16; ++i) {
            couriers.emplace_back([&r, &m, &served] {
                std::optional<Restaurant::Slot> slot = r.enter();
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
                    std::optional<Restaurant::Slot> slot = r.enter();
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
        std::optional<Restaurant::Slot> held = r.enter();
        std::mutex m;
        int refused = 0;
        std::vector<std::thread> couriers;
        for (int i = 0; i < 5; ++i) {
            couriers.emplace_back([&r, &m, &refused] {
                const bool got = r.enter().has_value();
                std::lock_guard<std::mutex> lock(m);
                refused += got ? 0 : 1;
            });
        }
        r.abort();
        for (std::thread& t : couriers) {
            t.join();  // si abort no despertara a todos, esto se colgaría
        }
        ok &= check("abort: las 5 motos en fila vuelven sin puesto", refused == 5);
        ok &= check("abort: una moto que llega después no espera", !r.enter());
        held.reset();
        ok &= check("abort: el puesto que estaba tomado se libera igual", r.inUse() == 0);
    }

    // 4. El puesto es move-only y se libera una sola vez, al destruirse su dueño final.
    {
        Restaurant r("r3", 2);
        std::optional<Restaurant::Slot> a = r.enter();
        Restaurant::Slot moved(std::move(*a));
        a.reset();  // el Slot vacío (movido) no libera nada
        ok &= check("move: sigue ocupado tras mover y destruir el original", r.inUse() == 1);
        {
            Restaurant::Slot last(std::move(moved));
        }
        ok &= check("move: se libera al destruirse el último dueño", r.inUse() == 0);
    }

    return ok ? 0 : 1;
}
