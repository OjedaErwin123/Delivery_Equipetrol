// Prueba de las versiones de CPU de S2 (EC2): la difusión de la demanda (Jacobi de 5 puntos con decaimiento). Una
// grilla en 0 sigue en 0; los bordes quedan en 0; sin los bordes cerca, un paso conserva la demanda multiplicada por
// kKeep (lo que sale de una celda entra en sus vecinas); la de N hilos (1 a 7) da exactamente lo de 1 hilo (las
// mismas operaciones en el mismo orden). Sale con 0 si todo está bien.
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

#include "diffusion.hpp"
#include "log_line.hpp"

int main()
{
    bool ok = true;
    const Grid zero{20, 10, std::vector<float>(200, 0.0f)};
    const Grid stillZero = diffuse(zero, 5);
    ok &= check("una grilla en 0 sigue en 0", std::all_of(stillZero.cells.begin(), stillZero.cells.end(),
                                                          [](float v) { return v == 0.0f; }));
    Grid point{41, 41, std::vector<float>(41 * 41, 0.0f)};
    point.cells[20 * 41 + 20] = 1000.0f;
    const Grid once = diffuse(point, 1);
    const double total = std::accumulate(once.cells.begin(), once.cells.end(), 0.0);
    ok &= check("lejos de los bordes, un paso conserva la demanda x kKeep (980 ± 1e-3)",
                std::fabs(total - 1000.0 * kKeep) < 1e-3);
    const Grid many = diffuse(point, 30);
    bool border = true;
    for (int i = 0; i < 41; ++i) {
        border &= many.at(i, 0) == 0.0f && many.at(i, 40) == 0.0f && many.at(0, i) == 0.0f && many.at(40, i) == 0.0f;
    }
    ok &= check("los bordes quedan en 0", border);

    std::mt19937 random(59);
    std::vector<std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : {1, 2, 3, 4, 7}) {
        pools.push_back(std::make_unique<ThreadPool>(t));
    }
    bool same = true;
    Grid in{65, 33, std::vector<float>(65 * 33)};
    for (float& c : in.cells) {
        c = static_cast<float>(random() % 40);
    }
    for (int steps : {0, 1, 9, 60}) {
        const Grid reference = diffuse(in, steps);
        for (auto& pool : pools) {
            same &= diffuseParallel(in, steps, *pool).cells == reference.cells;
        }
    }
    ok &= check("N hilos (1, 2, 3, 4 y 7) = 1 hilo, bit a bit, después de 0, 1, 9 y 60 pasos", same);
    return ok ? 0 : 1;
}
