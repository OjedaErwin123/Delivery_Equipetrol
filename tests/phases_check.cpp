// Prueba de las versiones de CPU de P1 (EC2): las fases de los pedidos. La de 1 hilo (sobre los registros, AoS)
// coincide con una cuenta a mano; la de N hilos (sobre las columnas, SoA; 1 a 7 hilos) da exactamente lo de 1 hilo;
// los pedidos sin entregar no cuentan; columnsOf copia los cuatro campos en orden. Sale con 0 si todo está bien.
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "log_line.hpp"
#include "phases.hpp"

int main()
{
    bool ok = true;
    std::vector<OrderRecord> two(2);
    two[0] = {1000, 31000, 151000, 451000, 3, 7, 0, 0};  // esperar 30 s, recoger 2 min, llevar 5 min
    two[1] = {2000, 5000, 9000, kNotYet, 1, 2, 0, 0};    // sin entregar: no cuenta
    const PhaseTotals hand = phaseTotals(two);
    ok &= check("a mano: 1 entregado, esperar 30 s, recoger 120 s, llevar 300 s",
                hand.delivered == 1 && hand.waitMs == 30000 && hand.pickupMs == 120000 && hand.rideMs == 300000);
    const OrderColumns cols = columnsOf(two);
    ok &= check("columnsOf: los cuatro campos, en orden", cols.createdMs == std::vector<std::uint32_t>{1000, 2000} &&
                                                              cols.deliveredMs[1] == kNotYet);

    std::mt19937 random(67);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool same = true;
    for (std::size_t n : {0, 1, 1000, 200000}) {
        std::vector<OrderRecord> records(n);
        for (OrderRecord& r : records) {
            r.createdMs = random() % 3600000;
            r.assignedMs = r.createdMs + random() % 600000;
            r.pickedUpMs = r.assignedMs + random() % 1200000;
            r.deliveredMs = random() % 5 == 0 ? kNotYet : r.pickedUpMs + random() % 1800000;
        }
        const PhaseTotals reference = phaseTotals(records);
        const OrderColumns columns = columnsOf(records);
        for (auto& pool : pools) {
            same &= phaseTotalsParallel(columns, *pool) == reference;
        }
    }
    ok &= check("N hilos sobre SoA (1, 2, 3, 4 y 7) = 1 hilo sobre AoS", same);
    return ok ? 0 : 1;
}
