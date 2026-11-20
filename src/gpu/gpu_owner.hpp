#pragma once

#include <cstddef>
#include <future>
#include <memory>
#include <vector>

#include "apsp.hpp"
#include "gpu/apsp_gpu.hpp"
#include "gpu/grouping_gpu.hpp"
#include "gpu/histogram_gpu.hpp"
#include "gpu/nearest_gpu.hpp"
#include "gpu/segment_stats_gpu.hpp"
#include "gpu/slot_histogram_gpu.hpp"
#include "thread_pool.hpp"

// EC2: el hilo dueño de la GPU. Toda llamada a CUDA (crear el contexto, reservar memoria, copiar, lanzar
// kernels, liberar) pasa por este único hilo: un ThreadPool de 1 hilo (la cola monitor de 07 + un promise por
// trabajo de 04, como el pool de estimaciones). Los demás hilos le mandan trabajos y esperan el future con el
// plazo que quieran (el despachador, quoteTimeoutMs): mientras corre un kernel, las motos siguen viajando y el
// bucle de frames sigue dibujando. Un error de CUDA llega por el future como excepción, no tira el programa.
//
// Por qué un solo hilo: el contexto de CUDA y los buffers reservados son estado de este objeto; si los tocaran
// varios hilos habría que protegerlos con un lock y se serializarían igual en la GPU. Así, `_nearest` lo crea,
// lo usa y lo destruye solo el hilo de `_thread` (ningún otro lo toca): no hay data race posible sobre él.
class GpuOwner {
public:
    // Crea el hilo y, en él, prepara la GPU para hasta `capacity` motos. Lanza si no hay GPU o falla la reserva.
    explicit GpuOwner(std::size_t capacity);
    // Libera la GPU en su hilo y después hace join del hilo (ThreadPool).
    ~GpuOwner();
    GpuOwner(const GpuOwner&) = delete;
    GpuOwner& operator=(const GpuOwner&) = delete;

    struct Nearest {
        std::vector<std::size_t> superset;  // posiciones de `at` (ver NearestGpu::superset)
        NearestGpu::Timing timing;
    };
    // Encola el prefiltro de las más cercanas. `at` viaja por valor: el trabajo puede terminar después de que
    // quien lo pidió dejó de esperar.
    std::future<ThreadPool::Outcome<Nearest>> nearest(std::vector<Coordinates> at, Coordinates shop, std::size_t limit);

    struct Apsp {
        DistanceTable table;  // resuelta
        ApspGpu::Timing timing;
    };
    // T1: encola Floyd–Warshall por bloques sobre `initial` (la tabla viaja por valor, ida y vuelta). La memoria
    // de la GPU se reserva en el primer pedido (o si llega una tabla más grande) y la tabla queda en la GPU.
    std::future<ThreadPool::Outcome<Apsp>> apsp(DistanceTable initial);

    struct HistogramResult {
        Histogram counts;
        HistogramGpu::Timing timing;
    };
    // H1: encola el histograma de los tiempos de entrega, privatizado en memoria compartida (`privatize`) o con
    // atómicos globales (para comparar). La memoria se reserva en el primer pedido o si llegan más datos.
    std::future<ThreadPool::Outcome<HistogramResult>> histogram(std::vector<std::uint32_t> ms, bool privatize);

    struct GroupResult {
        Grouping grouping;
        GroupingGpu::Timing timing;
    };
    // SC1: encola el agrupamiento estable de los pedidos por restaurante (counting sort con scan). Hasta
    // GroupingGpu::kMaxRestaurants restaurantes (si son más, el future trae la excepción). La memoria se reserva en
    // el primer pedido o si llegan más pedidos.
    std::future<ThreadPool::Outcome<GroupResult>> group(std::vector<std::uint32_t> restaurantOf, std::size_t restaurants);

    struct SegmentStatsResult {
        std::vector<RestaurantStats> stats;
        SegmentStatsGpu::Timing timing;
    };
    // R2: encola la reducción segmentada por restaurante (entregas, suma y máximo del tiempo de entrega) sobre los
    // grupos de SC1. La memoria se reserva en el primer pedido o si llegan más pedidos o restaurantes.
    std::future<ThreadPool::Outcome<SegmentStatsResult>> segmentStats(Grouping groups, std::vector<std::uint32_t> deliveryMs);

    struct SlotHistogramResult {
        SlotHistogram counts;
        SlotHistogramGpu::Timing timing;
    };
    // H2: encola el histograma 2D (restaurante, franja de 10 minutos) de los pedidos creados, con el recorrido
    // intercalado (coalescido, `interleaved`) o contiguo (para comparar). Hasta kMaxSlotRestaurants restaurantes
    // (si son más, el future trae la excepción). La memoria se reserva en el primer pedido o si llegan más pedidos.
    std::future<ThreadPool::Outcome<SlotHistogramResult>> slotHistogram(std::vector<std::uint32_t> restaurantOf,
                                                                        std::vector<std::uint32_t> createdMs,
                                                                        std::size_t restaurants, bool interleaved);

    // El nombre del dispositivo (leído en el hilo dueño al crear el objeto).
    const std::string& deviceName() const { return _deviceName; }

private:
    std::unique_ptr<NearestGpu> _nearest;  // solo lo toca el hilo de _thread
    std::unique_ptr<ApspGpu> _apsp;        // ídem
    std::size_t _apspStride = 0;           // ídem: el tamaño reservado para _apsp
    std::unique_ptr<HistogramGpu> _histogram;  // ídem
    std::size_t _histogramCapacity = 0;        // ídem
    std::unique_ptr<GroupingGpu> _grouping;    // ídem
    std::size_t _groupingCapacity = 0;         // ídem
    std::unique_ptr<SegmentStatsGpu> _segmentStats;  // ídem
    std::size_t _segmentCapacity = 0, _segmentRestaurants = 0;  // ídem
    std::unique_ptr<SlotHistogramGpu> _slotHistogram;           // ídem
    std::size_t _slotCapacity = 0;                              // ídem
    std::string _deviceName;
    ThreadPool _thread{1};                  // se destruye primero: join antes de que _nearest deje de existir
};
