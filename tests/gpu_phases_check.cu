// Prueba de igualdad de P1 (EC2): las fases de los pedidos en la GPU (real con USE_CUDA=ON, emulada con
// cuda_compat.h), desde los registros (AoS) y desde las columnas (SoA), dan EXACTAMENTE lo de la CPU (sumas de 64
// bits), con pedidos sin entregar, sin pedidos y con más pedidos que hilos de la grid. También por el hilo dueño de
// la GPU. Sale con 0 si todo está bien.
#include <cstdio>
#include <random>
#include <vector>

#include "gpu/cuda_compat.h"
#include "gpu/gpu_owner.hpp"
#include "gpu/phases_gpu.hpp"
#include "phases.hpp"

namespace {

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

std::vector<OrderRecord> randomOrders(std::size_t n, std::mt19937& random)
{
    std::vector<OrderRecord> records(n);
    for (OrderRecord& r : records) {
        r.createdMs = random() % 3600000;
        r.assignedMs = r.createdMs + random() % 600000;
        r.pickedUpMs = r.assignedMs + random() % 1200000;
        r.deliveredMs = random() % 5 == 0 ? kNotYet : r.pickedUpMs + random() % 1800000;
    }
    return records;
}

}  // namespace

int main()
{
    std::mt19937 random(71);
    const std::size_t big = BY_BACKEND(1000000, 3000);
    PhasesGpu gpu(big);
    bool same = true;
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{257}, big}) {
        const std::vector<OrderRecord> records = randomOrders(n, random);
        const PhaseTotals cpu = phaseTotals(records);
        same &= gpu.fromRecords(records) == cpu && gpu.fromColumns(columnsOf(records)) == cpu;
    }
    std::printf("hasta %zu pedidos (%s)\n", big, CUDA_BACKEND);
    bool ok = check("AoS y SoA en la GPU = CPU (entregados y las tres sumas)", same);

    GpuOwner owner(1);
    const std::vector<OrderRecord> records = randomOrders(2000, random);
    ok &= check("por el hilo dueño de la GPU: las mismas sumas",
                owner.phases(records, true).get().take().totals == phaseTotals(records));
    return ok ? 0 : 1;
}
