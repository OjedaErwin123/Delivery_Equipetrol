#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "apsp.hpp"
#include "coverage.hpp"
#include "diffusion.hpp"
#include "grouping.hpp"
#include "heatmap.hpp"
#include "histogram.hpp"
#include "nearest.hpp"
#include "segment_stats.hpp"
#include "slot_histogram.hpp"
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

// T1: la tabla de distancias de todos contra todos (Floyd–Warshall; por bloques en la GPU). Tamaño = nodos: los n
// más cercanos a la partida de las motos (la plaza), con las calles entre ellos; el último, la ciudad entera.
// Floyd–Warshall es O(n³): con la ciudad entera, la CPU tarda segundos por corrida; se hacen a lo sumo 10
// repeticiones (las que pide el documento).
void benchApsp(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "t1_apsp";
    const RoadGraph& graph = params.world->graph;
    const Coordinates center = graph.coordinates(params.world->startNode);
    std::vector<std::pair<double, std::size_t>> byDistance;
    for (std::size_t i = 0; i < graph.nodeCount(); ++i) {
        const Coordinates at = graph.coordinates(i);
        byDistance.emplace_back(haversineMeters(at.lat, at.lon, center.lat, center.lon), i);
    }
    std::sort(byDistance.begin(), byDistance.end());
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    const int reps = std::min(repetitions, 10);
    for (std::size_t n : {std::size_t{50}, std::size_t{150}, std::size_t{500}, std::size_t{1500}, graph.nodeCount()}) {
        std::vector<std::size_t> nodes;
        for (std::size_t k = 0; k < n; ++k) {
            nodes.push_back(byDistance[k].second);
        }
        const DistanceTable initial = initialTable(graph, nodes);
        DistanceTable reference = initial;
        floydWarshall(reference);
        auto runAll = [&](int rep) {
            DistanceTable table = initial;
            auto start = std::chrono::steady_clock::now();
            floydWarshall(table);
            double ms = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, ms, table.meters == reference.meters);
            }
            for (auto& pool : pools) {
                table = initial;
                start = std::chrono::steady_clock::now();
                floydWarshallBlocked(table, *pool.second);
                ms = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, ms, table.meters == reference.meters);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::Apsp result = gpu->apsp(initial).get().take();
            ms = msSince(start);
            const bool ok = result.table.meters == reference.meters;
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, ms, ok);
            }
#endif
        };
        runAll(-1);
        for (int rep = 0; rep < reps; ++rep) {
            runAll(rep);
        }
        progress << kApp << " n=" << n << ":" << csv.medians() << "\n";
    }
}

// H1: el histograma de los tiempos de entrega. Tamaño = entregas: de 10^3 (una corrida corta) a 10^7 (una corrida
// larga de la ciudad, o la repetición de muchos logs). Los tiempos son sintéticos con una forma parecida a la de la
// ciudad: la suma de tres sorteos uniformes de 5 a 60 minutos (semilla fija). Además de las cuatro versiones,
// gpu_kernel_global: el kernel ingenuo con un atómico global por entrega, para medir la contención.
void benchHistogram(const SimParams&, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "h1_delivery_times";
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (std::size_t n : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}, std::size_t{1000000},
                          std::size_t{10000000}}) {
        std::mt19937 random(static_cast<std::uint32_t>(7 + n));
        std::vector<std::uint32_t> ms(n);
        for (std::uint32_t& value : ms) {
            value = 0;
            for (int k = 0; k < 3; ++k) {
                value += 300000 + random() % 3300000;  // de 5 a 60 minutos, en ms
            }
        }
        const Histogram reference = deliveryHistogram(ms);
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = deliveryHistogram(ms) == reference;
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = deliveryHistogramParallel(ms, *pool.second) == reference;
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::HistogramResult result = gpu->histogram(ms, true).get().take();
            took = msSince(start);
            ok = result.counts == reference;
            const GpuOwner::HistogramResult naive = gpu->histogram(ms, false).get().take();
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
                csv.row(kApp, "gpu_kernel_global", 1, n, rep, naive.timing.kernelMs, naive.counts == reference);
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

// SC1: los pedidos agrupados por restaurante (counting sort estable con scan). Tamaño = pedidos: de 10^3 (una
// corrida corta) a 10^7. El restaurante de cada pedido es uno al azar de la configuración (37 en la ciudad; semilla
// fija), como los sortea el generador.
void benchGrouping(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "sc1_group";
    const std::size_t restaurants = params.restaurants.size();
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (std::size_t n : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}, std::size_t{1000000},
                          std::size_t{10000000}}) {
        std::mt19937 random(static_cast<std::uint32_t>(11 + n));
        std::vector<std::uint32_t> restaurantOf(n);
        for (std::uint32_t& r : restaurantOf) {
            r = static_cast<std::uint32_t>(random() % restaurants);
        }
        const Grouping reference = groupByRestaurant(restaurantOf, restaurants);
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = groupByRestaurant(restaurantOf, restaurants) == reference;
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = groupByRestaurantParallel(restaurantOf, restaurants, *pool.second) == reference;
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::GroupResult result = gpu->group(restaurantOf, restaurants).get().take();
            took = msSince(start);
            ok = result.grouping == reference;
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
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

// R2: por restaurante, entregas, suma y máximo del tiempo de entrega (reducción segmentada sobre los grupos de SC1).
// Tamaño = pedidos, de 10^3 a 10^7; restaurante al azar (37 en la ciudad), uno de cada cuatro sin entregar y los
// demás con un tiempo de 1 a 121 minutos (semilla fija). El agrupamiento se hace una vez, fuera de la medición: se
// mide solo la reducción.
void benchSegmentStats(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "r2_per_restaurant";
    const std::size_t restaurants = params.restaurants.size();
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (std::size_t n : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}, std::size_t{1000000},
                          std::size_t{10000000}}) {
        std::mt19937 random(static_cast<std::uint32_t>(17 + n));
        std::vector<std::uint32_t> restaurantOf(n), deliveryMs(n);
        for (std::size_t i = 0; i < n; ++i) {
            restaurantOf[i] = static_cast<std::uint32_t>(random() % restaurants);
            deliveryMs[i] = random() % 4 == 0 ? kNotDelivered : 60000 + random() % 7200000;
        }
        const Grouping groups = groupByRestaurant(restaurantOf, restaurants);
        const std::vector<RestaurantStats> reference = statsPerRestaurant(groups, deliveryMs);
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = statsPerRestaurant(groups, deliveryMs) == reference;
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = statsPerRestaurantParallel(groups, deliveryMs, *pool.second) == reference;
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::SegmentStatsResult result = gpu->segmentStats(groups, deliveryMs).get().take();
            took = msSince(start);
            ok = result.stats == reference;
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
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

// H2: los pedidos creados por (restaurante, franja de 10 minutos), histograma 2D privatizado. Tamaño = pedidos, de
// 10^3 a 10^7; restaurante al azar (37 en la ciudad) y hora de creación uniforme en 24 h (semilla fija). Además de
// las cuatro versiones, gpu_kernel_contiguous: el mismo kernel con el recorrido contiguo por hilo (no coalescido),
// para medir el efecto del acceso a memoria (carpeta 04).
void benchSlotHistogram(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "h2_hour_restaurant";
    const std::size_t restaurants = params.restaurants.size();
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (std::size_t n : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}, std::size_t{1000000},
                          std::size_t{10000000}}) {
        std::mt19937 random(static_cast<std::uint32_t>(19 + n));
        std::vector<std::uint32_t> restaurantOf(n), createdMs(n);
        for (std::size_t i = 0; i < n; ++i) {
            restaurantOf[i] = static_cast<std::uint32_t>(random() % restaurants);
            createdMs[i] = random() % (kSlots * kSlotMs);
        }
        const SlotHistogram reference = ordersPerSlot(restaurantOf, createdMs, restaurants);
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = ordersPerSlot(restaurantOf, createdMs, restaurants) == reference;
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = ordersPerSlotParallel(restaurantOf, createdMs, restaurants, *pool.second) == reference;
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::SlotHistogramResult result =
                gpu->slotHistogram(restaurantOf, createdMs, restaurants, true).get().take();
            took = msSince(start);
            ok = result.counts == reference;
            const GpuOwner::SlotHistogramResult contiguous =
                gpu->slotHistogram(restaurantOf, createdMs, restaurants, false).get().take();
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
                csv.row(kApp, "gpu_kernel_contiguous", 1, n, rep, contiguous.timing.kernelMs,
                        contiguous.counts == reference);
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

// C1: el mapa de calor, convolución gaussiana de 13 x 13. Tamaño = celdas de la grilla: lados de 64 a 2048 (de 4096 a
// 4,2 millones de celdas, tres órdenes de magnitud). La grilla de entrada es dispersa como la de la ciudad: una celda
// de cada 50 con pedidos (de 1 a 20; semilla fija). Correcto = dentro de 1e-5 del mayor valor de la referencia
// (flotantes; ver heatmap.hpp). Además de las cuatro versiones, gpu_kernel_basic: la convolución sin tiling (cada
// hilo lee sus 169 vecinos de la memoria global).
void benchHeatmap(const SimParams&, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "c1_heatmap";
    const std::vector<float> filter = gaussianFilter();
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (int side : {64, 128, 256, 512, 1024, 2048}) {
        std::mt19937 random(static_cast<std::uint32_t>(23 + side));
        Grid in;
        in.width = in.height = side;
        in.cells.assign(static_cast<std::size_t>(side) * side, 0.0f);
        for (float& cell : in.cells) {
            if (random() % 50 == 0) {
                cell = static_cast<float>(1 + random() % 20);
            }
        }
        const std::size_t n = in.cells.size();
        const Grid reference = convolve(in, filter);
        const float tolerance = 1e-5f * maxValue(reference);
        auto close = [&](const Grid& g) { return maxDifference(g, reference) <= tolerance; };
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = close(convolve(in, filter));
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                const Grid out = convolveParallel(in, filter, *pool.second);
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, close(out));
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::HeatmapResult result = gpu->heatmap(in, filter, true).get().take();
            took = msSince(start);
            ok = close(result.grid);
            const GpuOwner::HeatmapResult basic = gpu->heatmap(in, filter, false).get().take();
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
                csv.row(kApp, "gpu_kernel_basic", 1, n, rep, basic.timing.kernelMs, close(basic.grid));
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

// S1: el mapa de cobertura, barridos de Jacobi de un stencil de 5 puntos. Tamaño = celdas: lados de 32 a 1024 (de
// 1024 a 1 millón de celdas, tres órdenes de magnitud), con 100 barridos por corrida en todos los tamaños y versiones
// (así el trabajo crece solo con las celdas; el reporte, en cambio, barre hasta converger). Dos de cada tres celdas con calles; 37 restaurantes en celdas al azar (semilla fija). Enteros: correcto =
// idéntico. Además de las cuatro versiones, gpu_kernel_basic: el stencil sin tiling (4 vecinos de la memoria global).
void benchCoverage(const SimParams& params, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "s1_coverage";
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (int side : {32, 64, 128, 256, 512, 1024}) {
        std::mt19937 random(static_cast<std::uint32_t>(29 + side));
        const std::size_t n = static_cast<std::size_t>(side) * side;
        CoverageGrid grid{side, side, std::vector<std::uint32_t>(n), std::vector<std::uint32_t>(n, kFar)};
        for (std::uint32_t& c : grid.cost) {
            c = random() % 3 == 0 ? kOffStreetCost : kStreetCost;
        }
        for (std::size_t k = 0; k < params.restaurants.size(); ++k) {
            grid.distance[random() % n] = 0;
        }
        const int sweeps = 100;
        const std::vector<std::uint32_t> reference = relaxCoverage(grid, sweeps);
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = relaxCoverage(grid, sweeps) == reference;
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                ok = relaxCoverageParallel(grid, sweeps, *pool.second) == reference;
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, ok);
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::CoverageResult result = gpu->coverage(grid, sweeps, true).get().take();
            took = msSince(start);
            ok = result.distance == reference;
            const GpuOwner::CoverageResult basic = gpu->coverage(grid, sweeps, false).get().take();
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
                csv.row(kApp, "gpu_kernel_basic", 1, n, rep, basic.timing.kernelMs, basic.distance == reference);
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

// S2: la difusión de la demanda, 100 pasos de Jacobi de 5 puntos en flotantes por corrida. Tamaño = celdas: lados de
// 64 a 2048 (de 4096 a 4,2 millones de celdas, tres órdenes de magnitud). Grilla inicial dispersa como la de C1 (una
// celda de cada 50 con pedidos; semilla fija). Correcto = dentro de 1e-5 del mayor valor inicial (flotantes con FMA;
// ver diffusion.hpp). Además de las cuatro versiones, gpu_kernel_basic: el stencil sin coarsening.
void benchDiffusion(const SimParams&, Csv& csv, std::ostream& progress, int repetitions, GpuOwner* gpu)
{
    static const char* kApp = "s2_diffusion";
    constexpr int kSteps = 100;
    std::map<std::size_t, std::unique_ptr<ThreadPool>> pools;
    for (std::size_t t : threadCounts()) {
        pools[t] = std::make_unique<ThreadPool>(t);
    }
#ifndef DELIVERY_USE_CUDA
    (void)gpu;
#endif
    for (int side : {64, 128, 256, 512, 1024, 2048}) {
        std::mt19937 random(static_cast<std::uint32_t>(31 + side));
        Grid initial;
        initial.width = initial.height = side;
        initial.cells.assign(static_cast<std::size_t>(side) * side, 0.0f);
        for (float& cell : initial.cells) {
            if (random() % 50 == 0) {
                cell = static_cast<float>(1 + random() % 20);
            }
        }
        const std::size_t n = initial.cells.size();
        const Grid reference = diffuse(initial, kSteps);
        const float tolerance = 1e-5f * maxValue(initial);
        auto close = [&](const Grid& g) { return maxDifference(g, reference) <= tolerance; };
        auto runAll = [&](int rep) {
            auto start = std::chrono::steady_clock::now();
            bool ok = close(diffuse(initial, kSteps));
            double took = msSince(start);
            if (rep >= 0) {
                csv.row(kApp, "cpu_1", 1, n, rep, took, ok);
            }
            for (auto& pool : pools) {
                start = std::chrono::steady_clock::now();
                const Grid out = diffuseParallel(initial, kSteps, *pool.second);
                took = msSince(start);
                if (rep >= 0) {
                    csv.row(kApp, "cpu_threads", pool.first, n, rep, took, close(out));
                }
            }
#ifdef DELIVERY_USE_CUDA
            start = std::chrono::steady_clock::now();
            const GpuOwner::DiffusionResult result = gpu->diffusion(initial, kSteps, true).get().take();
            took = msSince(start);
            ok = close(result.grid);
            const GpuOwner::DiffusionResult basic = gpu->diffusion(initial, kSteps, false).get().take();
            if (rep >= 0) {
                csv.row(kApp, "gpu_kernel", 1, n, rep, result.timing.kernelMs, ok);
                csv.row(kApp, "gpu_end_to_end", 1, n, rep, result.timing.endToEndMs, ok);
                csv.row(kApp, "gpu_via_owner", 1, n, rep, took, ok);
                csv.row(kApp, "gpu_kernel_basic", 1, n, rep, basic.timing.kernelMs, close(basic.grid));
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

int runBenchmarks(const SimParams& params, std::ostream& csv, std::ostream& progress, int repetitions,
                  const std::string& only)
{
    using Bench = void (*)(const SimParams&, Csv&, std::ostream&, int, GpuOwner*);
    const std::vector<std::pair<std::string, Bench>> apps = {
        {"r1_nearest", benchNearest},
        {"t1_apsp", benchApsp},
        {"h1_delivery_times", benchHistogram},
        {"sc1_group", benchGrouping},
        {"r2_per_restaurant", benchSegmentStats},
        {"h2_hour_restaurant", benchSlotHistogram},
        {"c1_heatmap", benchHeatmap},
        {"s1_coverage", benchCoverage},
        {"s2_diffusion", benchDiffusion},
    };
    if (!only.empty() &&
        std::none_of(apps.begin(), apps.end(), [&only](const auto& app) { return app.first == only; })) {
        std::string names;
        for (const auto& app : apps) {
            names += (names.empty() ? "" : ", ") + app.first;
        }
        throw std::invalid_argument("no hay ninguna aplicación '" + only + "' (" + names + ")");
    }
    std::string machine = cpuModel() + " x" + std::to_string(ThreadPool::defaultSize());
#ifdef DELIVERY_USE_CUDA
    GpuOwner gpu(kBenchMaxSize);  // toda llamada a CUDA, en su hilo
    machine += " | " + gpu.deviceName();
#endif
    progress << "bench: " << machine << ", " << repetitions << " repeticiones por tamaño\n";
    Csv table(csv, machine);
#ifdef DELIVERY_USE_CUDA
    GpuOwner* device = &gpu;
#else
    GpuOwner* device = nullptr;
#endif
    for (const auto& app : apps) {
        if (only.empty() || app.first == only) {
            app.second(params, table, progress, repetitions, device);
        }
    }
    return table.allCorrect() ? 0 : 1;
}
