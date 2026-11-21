// Prueba de las versiones de CPU de C1 (EC2): la convolución gaussiana del mapa de calor. El filtro suma 1 y es
// simétrico; un impulso lejos de los bordes queda repartido conservando el total (dentro del redondeo de float); la
// de N hilos (1 a 7) da exactamente lo de 1 hilo (la misma suma, en el mismo orden, celda por celda); las grillas
// que no son múltiplo de 16 y las de una sola fila también. Sale con 0 si todo está bien.
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "heatmap.hpp"
#include "log_line.hpp"

int main()
{
    const std::vector<float> filter = gaussianFilter();
    const double total = std::accumulate(filter.begin(), filter.end(), 0.0);
    bool symmetric = true;
    for (int fy = 0; fy < kTaps; ++fy) {
        for (int fx = 0; fx < kTaps; ++fx) {
            symmetric &= filter[fy * kTaps + fx] == filter[fx * kTaps + fy] &&
                         filter[fy * kTaps + fx] == filter[(kTaps - 1 - fy) * kTaps + (kTaps - 1 - fx)];
        }
    }
    bool ok = true;
    ok &= check("el filtro suma 1 (±1e-6) y es simétrico", std::fabs(total - 1.0) < 1e-6 && symmetric);

    Grid impulse{64, 64, std::vector<float>(64 * 64, 0.0f)};
    impulse.cells[32 * 64 + 32] = 100.0f;  // 100 pedidos en una celda, lejos de los bordes
    const Grid spread = convolve(impulse, filter);
    const double after = std::accumulate(spread.cells.begin(), spread.cells.end(), 0.0);
    ok &= check("un impulso lejos de los bordes conserva el total (100 ± 1e-3)", std::fabs(after - 100.0) < 1e-3);

    std::mt19937 random(41);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool same = true;
    for (auto size : {std::pair<int, int>{1, 1}, {37, 1}, {50, 33}, {128, 128}}) {
        Grid in{size.first, size.second, std::vector<float>(static_cast<std::size_t>(size.first) * size.second)};
        for (float& c : in.cells) {
            c = static_cast<float>(random() % 20);
        }
        const Grid reference = convolve(in, filter);
        for (auto& pool : pools) {
            same &= convolveParallel(in, filter, *pool).cells == reference.cells;
        }
    }
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo, bit a bit (1x1, 37x1, 50x33, 128x128)", same);
    return ok ? 0 : 1;
}
