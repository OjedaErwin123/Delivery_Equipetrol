#include "gpu/gpu_owner.hpp"

#include <algorithm>
#include <string>
#include <utility>

GpuOwner::GpuOwner(std::size_t capacity)
{
    // Se espera el resultado: así un error (sin GPU, sin memoria) sale de este constructor, en el hilo que lo creó.
    _deviceName = _thread
                      .submit([this, capacity] {
                          _nearest = std::make_unique<NearestGpu>(capacity);
                          return NearestGpu::deviceName();
                      })
                      .get()
                      .take();
}

GpuOwner::~GpuOwner()
{
    // La memoria de la GPU se libera en el hilo dueño. Los trabajos anteriores ya salieron de la cola (es FIFO y
    // de un solo hilo): este corre después de todos.
    _thread
        .submit([this] {
            _nearest.reset();
            _apsp.reset();
            _histogram.reset();
            _grouping.reset();
            _segmentStats.reset();
            _slotHistogram.reset();
            _heatmap.reset();
            _coverage.reset();
            _diffusion.reset();
            return true;
        })
        .get();
}

std::future<ThreadPool::Outcome<GpuOwner::Nearest>> GpuOwner::nearest(std::vector<Coordinates> at, Coordinates shop,
                                                                      std::size_t limit)
{
    return _thread.submit([this, at = std::move(at), shop, limit] {
        Nearest result;
        result.superset = _nearest->superset(at, shop, limit, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::Apsp>> GpuOwner::apsp(DistanceTable initial)
{
    return _thread.submit([this, table = std::move(initial)]() mutable {
        if (!_apsp || table.stride > _apspStride) {
            _apsp.reset();  // primero liberar: la tabla vieja y la nueva no tienen que entrar juntas
            _apsp = std::make_unique<ApspGpu>(table.stride);
            _apspStride = table.stride;
        }
        Apsp result;
        _apsp->solve(table, &result.timing);
        result.table = std::move(table);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::HistogramResult>> GpuOwner::histogram(std::vector<std::uint32_t> ms,
                                                                                bool privatize)
{
    return _thread.submit([this, ms = std::move(ms), privatize] {
        if (!_histogram || ms.size() > _histogramCapacity) {
            _histogram.reset();
            _histogramCapacity = std::max<std::size_t>(ms.size(), 1);
            _histogram = std::make_unique<HistogramGpu>(_histogramCapacity);
        }
        HistogramResult result;
        result.counts = privatize ? _histogram->privatized(ms, &result.timing) : _histogram->globalAtomics(ms, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::GroupResult>> GpuOwner::group(std::vector<std::uint32_t> restaurantOf,
                                                                        std::size_t restaurants)
{
    return _thread.submit([this, restaurantOf = std::move(restaurantOf), restaurants] {
        if (!_grouping || restaurantOf.size() > _groupingCapacity) {
            _grouping.reset();
            _groupingCapacity = std::max<std::size_t>(restaurantOf.size(), 1);
            _grouping = std::make_unique<GroupingGpu>(_groupingCapacity);
        }
        GroupResult result;
        result.grouping = _grouping->group(restaurantOf, restaurants, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::SegmentStatsResult>> GpuOwner::segmentStats(Grouping groups,
                                                                                      std::vector<std::uint32_t> deliveryMs)
{
    return _thread.submit([this, groups = std::move(groups), deliveryMs = std::move(deliveryMs)] {
        const std::size_t restaurants = groups.offsets.empty() ? 0 : groups.offsets.size() - 1;
        if (!_segmentStats || deliveryMs.size() > _segmentCapacity || restaurants > _segmentRestaurants) {
            _segmentStats.reset();
            _segmentCapacity = std::max(deliveryMs.size(), _segmentCapacity);
            _segmentCapacity = std::max<std::size_t>(_segmentCapacity, 1);
            _segmentRestaurants = std::max<std::size_t>({restaurants, _segmentRestaurants, 64});
            _segmentStats = std::make_unique<SegmentStatsGpu>(_segmentCapacity, _segmentRestaurants);
        }
        SegmentStatsResult result;
        result.stats = _segmentStats->stats(groups, deliveryMs, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::SlotHistogramResult>> GpuOwner::slotHistogram(
    std::vector<std::uint32_t> restaurantOf, std::vector<std::uint32_t> createdMs, std::size_t restaurants,
    bool interleaved)
{
    return _thread.submit([this, restaurantOf = std::move(restaurantOf), createdMs = std::move(createdMs), restaurants,
                           interleaved] {
        if (!_slotHistogram || restaurantOf.size() > _slotCapacity) {
            _slotHistogram.reset();
            _slotCapacity = std::max<std::size_t>(restaurantOf.size(), 1);
            _slotHistogram = std::make_unique<SlotHistogramGpu>(_slotCapacity);
        }
        SlotHistogramResult result;
        result.counts = _slotHistogram->count(restaurantOf, createdMs, restaurants, interleaved, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::HeatmapResult>> GpuOwner::heatmap(Grid in, std::vector<float> filter,
                                                                            bool tiled)
{
    return _thread.submit([this, in = std::move(in), filter = std::move(filter), tiled] {
        if (!_heatmap || in.cells.size() > _heatmapCells) {
            _heatmap.reset();
            _heatmapCells = std::max<std::size_t>(in.cells.size(), 1);
            _heatmap = std::make_unique<HeatmapGpu>(_heatmapCells);
        }
        HeatmapResult result;
        result.grid = _heatmap->convolve(in, filter, tiled, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::CoverageResult>> GpuOwner::coverage(CoverageGrid grid, int sweeps, bool tiled)
{
    return _thread.submit([this, grid = std::move(grid), sweeps, tiled] {
        if (!_coverage || grid.cost.size() > _coverageCells) {
            _coverage.reset();
            _coverageCells = std::max<std::size_t>(grid.cost.size(), 1);
            _coverage = std::make_unique<CoverageGpu>(_coverageCells);
        }
        CoverageResult result;
        result.distance = _coverage->relax(grid, sweeps, tiled, &result.timing);
        return result;
    });
}

std::future<ThreadPool::Outcome<GpuOwner::DiffusionResult>> GpuOwner::diffusion(Grid start, int steps, bool coarsened)
{
    return _thread.submit([this, start = std::move(start), steps, coarsened] {
        if (!_diffusion || start.cells.size() > _diffusionCells) {
            _diffusion.reset();
            _diffusionCells = std::max<std::size_t>(start.cells.size(), 1);
            _diffusion = std::make_unique<DiffusionGpu>(_diffusionCells);
        }
        DiffusionResult result;
        result.grid = _diffusion->diffuse(start, steps, coarsened, &result.timing);
        return result;
    });
}
