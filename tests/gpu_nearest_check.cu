// Prueba de igualdad de R1 (EC2): el prefiltro de las motos más cercanas en la GPU. En una GPU real (USE_CUDA=ON) o
// emulado en la CPU (cuda_compat.h): el subconjunto de la GPU tiene más de `limit` motos y contiene a las `limit`
// mejores de la CPU, y la decisión de la CPU dentro de él (nearestWithin) es EXACTAMENTE la de nearestOf sobre
// todas, también con muchos empates (varias motos en el mismo nodo). Lo mismo a través del hilo dueño de la GPU
// (GpuOwner), y un error (más motos que la capacidad) llega por el future como excepción, sin tirar el programa.
// Sale con 0 si todo está bien.
#include <algorithm>
#include <cstdio>
#include <future>
#include <iostream>
#include <random>
#include <vector>

#include "gpu/gpu_owner.hpp"
#include "gpu/nearest_gpu.hpp"
#include "nearest.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    std::mt19937 random(11);
    std::vector<Coordinates> points;  // 60 nodos posibles en ~3 km: muchas motos comparten distancia
    for (int k = 0; k < 60; ++k) {
        points.push_back({-17.81 + (random() % 3000) * 1e-5, -63.21 + (random() % 3000) * 1e-5});
    }
    const std::size_t capacity = 2000;
    NearestGpu gpu(capacity);
    std::printf("dispositivo: %s\n", NearestGpu::deviceName().c_str());
    bool bigger = true, contains = true, same = true;
    std::size_t largestSuperset = 0;
    for (std::size_t n : {1, 8, 9, 10, 100, 257, 1000, 2000}) {
        for (std::size_t limit : {1, 8}) {
            std::vector<Coordinates> at(n);
            std::vector<std::size_t> ids(n);
            for (std::size_t i = 0; i < n; ++i) {
                at[i] = points[random() % points.size()];
                ids[i] = i;
            }
            const Coordinates shop = points[random() % points.size()];
            const std::vector<std::size_t> reference = nearestOf(at, ids, shop, limit);
            const std::vector<std::size_t> superset = gpu.superset(at, shop, limit);
            if (n > limit) {
                bigger &= superset.size() > limit;
                for (std::size_t id : reference) {
                    contains &= std::binary_search(superset.begin(), superset.end(), id);
                }
                same &= nearestWithin(at, ids, superset, shop, limit) == reference;
                largestSuperset = std::max(largestSuperset, superset.size());
            } else {
                same &= superset == ids;
            }
        }
    }
    std::printf("subconjunto más grande devuelto: %zu motos (los empates en distancia entran todos)\n", largestSuperset);
    bool ok = true;
    ok &= check("el subconjunto de la GPU tiene más de `limit` motos", bigger);
    ok &= check("el subconjunto contiene a las `limit` más cercanas de la CPU (con empates)", contains);
    ok &= check("la decisión de la CPU dentro del subconjunto = nearestOf sobre todas", same);

    // A través del hilo dueño de la GPU: el mismo resultado, y un error llega por el future.
    {
        GpuOwner owner(capacity);
        std::vector<Coordinates> at(500);
        std::vector<std::size_t> ids(500);
        for (std::size_t i = 0; i < at.size(); ++i) {
            at[i] = points[random() % points.size()];
            ids[i] = i;
        }
        const Coordinates shop = points[0];
        const GpuOwner::Nearest result = owner.nearest(at, shop, 8).get().take();
        ok &= check("por el hilo dueño: misma decisión que nearestOf",
                    nearestWithin(at, ids, result.superset, shop, 8) == nearestOf(at, ids, shop, 8));
        bool errorArrived = false;
        try {
            owner.nearest(std::vector<Coordinates>(capacity + 1), shop, 8).get().take();
        } catch (const std::exception& error) {
            errorArrived = true;
            std::printf("error esperado: \"%s\"\n", error.what());
        }
        ok &= check("un error de la GPU llega por el future como excepción", errorArrived);
        ok &= check("después del error, el hilo dueño sigue atendiendo",
                    owner.nearest(at, shop, 8).get().take().superset == result.superset);
    }
    return ok ? 0 : 1;
}
