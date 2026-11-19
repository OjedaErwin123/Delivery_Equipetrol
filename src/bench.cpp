#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "nearest.hpp"
#include "thread_pool.hpp"
#ifdef DELIVERY_USE_CUDA
#include "gpu/gpu_owner.hpp"
#endif

#ifndef DELIVERY_USE_CUDA
class GpuOwner;  // sin CUDA no hay GPU: las funciones reciben nullptr
#endif

namespace {

constexpr std::size_t kBenchMaxSize = 100000;  // el tamaño más grande del barrido (reserva de la GPU)

// El modelo de la CPU (Linux y WSL: /proc/cpuinfo), para la columna machine.
std::string cpuModel()
{
    std::ifstream info("/proc/cpuinfo");
    for (std::string line; std::getline(info, line);) {
        if (line.rfind("model name", 0) == 0) {
            const auto colon = line.find(':');
            return colon == std::string::npos ? line : line.substr(colon + 2);
        }
    }
    return "CPU desconocida";
}

double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

double median(std::vector<double> values)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}

// 1, 2, 4 … y hardware_concurrency() si no es potencia de 2.
std::vector<std::size_t> threadCounts()
{
    const std::size_t most = ThreadPool::defaultSize();
    std::vector<std::size_t> counts;
    for (std::size_t t = 1; t <= most; t *= 2) {
        counts.push_back(t);
    }
    if (counts.back() != most) {
        counts.push_back(most);
    }
    return counts;
}

class Csv {
public:
    Csv(std::ostream& out, std::string machine) : _out(out), _machine(std::move(machine))
    {
        _out << "application,version,threads,size,repetition,ms,correct,machine\n";
    }
    void row(const char* application, const char* version, std::size_t threads, std::size_t size, int repetition,
             double ms, bool correct)
    {
        _out << application << ',' << version << ',' << threads << ',' << size << ',' << repetition << ',' << ms << ','
             << (correct ? "yes" : "no") << ",\"" << _machine << "\"\n";
        _allCorrect = _allCorrect && correct;
        _ms[std::string(version) + (std::string(version) == "cpu_threads" ? "x" + std::to_string(threads) : "")]
            .push_back(ms);
    }
    bool allCorrect() const { return _allCorrect; }
    // Las medianas de lo registrado desde la última llamada (para el progreso), y vacía la cuenta.
    std::string medians()
    {
        std::string text;
        for (const auto& entry : _ms) {
            text += " " + entry.first + "=" + std::to_string(median(entry.second)) + "ms";
        }
        _ms.clear();
        return text;
    }

private:
    std::ostream& _out;
    std::string _machine;
    bool _allCorrect = true;
    std::map<std::string, std::vector<double>> _ms;
};

// R1: las motos más cercanas al restaurante (reducción). Tamaño = cantidad de motos candidatas. Cada moto está en
// un nodo al azar de la ciudad (puede haber varias en el mismo nodo: los empates se prueban de verdad) y el
// restaurante es uno al azar de la configuración.
void benchNearest(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "r1_nearest";
    const std::vector<std::size_t> sizes = {100, 300, 1000, 3000, 10000, 30000, kBenchMaxSize};
    const std::size_t limit = kNearestCandidates;
    const World& world = *params.world;
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;  // sin CUDA, solo las versiones de CPU
#endif
    for (std::size_t n : sizes) {
        std::mt19937 random(static_cast<std::uint32_t>(42 + n));
        std::vector<Coordinates> at(n);
        std::vector<std::size_t> ids(n);
        for (std::size_t i = 0; i < n; ++i) {
            at[i] = world.graph.coordinates(random() % world.graph.nodeCount());
            ids[i] = i;
        }
        const std::string& restaurant = params.restaurants[random() % params.restaurants.size()];
        const Coordinates shop = world.graph.coordinates(world.restaurantNode.at(restaurant));
        const std::vector<std::size_t> reference = nearestOf(at, ids, shop, limit);

        // Una corrida de cada versión; rep < 0 = calentamiento (no se registra).
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = nearestOf(at, ids, shop, limit) == reference;
            double ms = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, ms, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = nearestOfParallel(at, ids, shop, limit, *pool.second) == reference;
                ms = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, ms, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::Nearest result = gpu->nearest(at, shop, limit).get().take();
            ms = msSince(start);
            ok = nearestWithin(at, ids, result.superset, shop, limit) == reference;
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, ms, ok);
            }
#endif
        };
        runAll(-1);
        for (int rep = 0; rep < repetitions; ++rep) {
            runAll(rep);
        }
        progress << kApp << " n=" << n << ":" << csv.medians() << "\n";
    }
}

}  // namespace

int runBenchmarks(const SimParams& params, std::ostream& csv, std::ostream& progress, int repetitions)
{
    std::string machine = cpuModel() + " x" + std::to_string(ThreadPool::defaultSize());
#ifdef DELIVERY_USE_CUDA
    GpuOwner gpu(kBenchMaxSize);  // toda llamada a CUDA, en su hilo
    machine += " | " + gpu.deviceName();
#endif
    progress << "bench: " << machine << ", " << repetitions << " repeticiones por tamaño\n";
    Csv table(csv, machine);
#ifdef DELIVERY_USE_CUDA
    benchNearest(params, table, progress, repetitions, &gpu);
#else
    benchNearest(params, table, progress, repetitions, nullptr);
#endif
    return table.allCorrect() ? 0 : 1;
}
