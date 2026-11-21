// Prueba de las versiones de CPU de P2 (EC2): el rodeo de los sentidos únicos. Una tabla a mano (un par asimétrico,
// uno simétrico, uno de un solo sentido y uno sin camino); el desempate del peor par (menor i, después menor j); la
// ingenua, la de bloques y la de N hilos (1, 2, 3, 4 y 7) dan exactamente lo mismo en tablas sintéticas que no son
// múltiplo de 32. Sale con 0 si todo está bien.
#include <iostream>
#include <memory>
#include <vector>

#include "asymmetry.hpp"
#include "log_line.hpp"

namespace {

DistanceTable byHand()
{
    // 4 nodos: 0 -> 1 = 100, 1 -> 0 = 150 (rodeo 50); 0 <-> 2 = 200 en los dos sentidos; 1 -> 2 sin camino, 2 -> 1 =
    // 50 (un solo sentido); 3 sin camino con nadie.
    const std::uint32_t X = DistanceTable::kUnreachable;
    DistanceTable t;
    t.n = 4;
    t.stride = 32;
    t.meters.assign(32 * 32, X);
    const std::uint32_t d[4][4] = {{0, 100, 200, X}, {150, 0, X, X}, {200, 50, 0, X}, {X, X, X, 0}};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            t.meters[i * 32 + j] = d[i][j];
        }
    }
    return t;
}

}  // namespace

int main()
{
    bool ok = true;
    const Asymmetry hand = asymmetryNaive(byHand());
    ok &= check("a mano: 2 pares con los dos sentidos, 1 asimétrico (50 m), 1 de un solo sentido",
                hand.pairs == 2 && hand.asymmetric == 1 && hand.sumDiff == 50 && hand.oneWay == 1 &&
                    hand.maxDiff() == 50 && hand.worstI() == 0 && hand.worstJ() == 1);
    ok &= check("a mano: por bloques = ingenua", asymmetryBlocked(byHand()) == hand);

    // Dos pares con el mismo rodeo máximo: gana el de menor i.
    DistanceTable tie = byHand();
    tie.meters[2 * 32 + 3] = 10;
    tie.meters[3 * 32 + 2] = 60;  // (2, 3) también con rodeo 50
    const Asymmetry tied = asymmetryNaive(tie);
    ok &= check("empate en el peor par: gana el menor i", tied.maxDiff() == 50 && tied.worstI() == 0 &&
                                                              tied.worstJ() == 1 && tied.asymmetric == 2);

    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool same = true;
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{31}, std::size_t{33}, std::size_t{100},
                          std::size_t{257}}) {
        const DistanceTable table = syntheticTable(n, static_cast<std::uint32_t>(5 + n), 7);  // muchos empates
        const Asymmetry naive = asymmetryNaive(table);
        same &= asymmetryBlocked(table) == naive;
        for (auto& pool : pools) {
            same &= asymmetryParallel(table, *pool) == naive;
        }
        if (n == 257) {
            std::cout << "257 nodos: " << naive.pairs << " pares, " << naive.asymmetric << " asimétricos, "
                      << naive.oneWay << " de un solo sentido, rodeo máximo " << naive.maxDiff() << " m\n";
            same &= naive.asymmetric > 0 && naive.oneWay > 0;
        }
    }
    ok &= check("ingenua = por bloques = N hilos (1, 2, 3, 4 y 7), con 0 a 257 nodos", same);
    return ok ? 0 : 1;
}
