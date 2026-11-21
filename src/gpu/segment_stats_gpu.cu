// EC2, R2: la reducción segmentada por restaurante en la GPU (ver segment_stats_gpu.hpp).
#include "gpu/segment_stats_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kThreads = 256;  // 8 warps por restaurante; potencia de 2 para el árbol
constexpr unsigned kNone = 0xffffffffu;

// Un bloque por restaurante (blockIdx.x). out: tres arreglos de `restaurants` (cantidad, suma, máximo).
__global__ void segmentStats(const unsigned* order, const unsigned* deliveryMs, const unsigned* offsets,
                             unsigned* delivered, unsigned long long* totalMs, unsigned* maxMs)
{
    __shared__ unsigned sCount[kThreads];
    __shared__ unsigned long long sSum[kThreads];
    __shared__ unsigned sMax[kThreads];
    const unsigned r = blockIdx.x;
    unsigned count = 0, most = 0;
    unsigned long long sum = 0;
    for (unsigned k = offsets[r] + threadIdx.x; k < offsets[r + 1]; k += blockDim.x) {
        const unsigned ms = deliveryMs[order[k]];  // lectura indirecta: el grupo es una permutación de los pedidos
        if (ms != kNone) {
            ++count;
            sum += ms;
            most = ms > most ? ms : most;
        }
    }
    sCount[threadIdx.x] = count;
    sSum[threadIdx.x] = sum;
    sMax[threadIdx.x] = most;
    __syncthreads();
    for (unsigned s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s) {
            sCount[threadIdx.x] += sCount[threadIdx.x + s];
            sSum[threadIdx.x] += sSum[threadIdx.x + s];
            sMax[threadIdx.x] = sMax[threadIdx.x + s] > sMax[threadIdx.x] ? sMax[threadIdx.x + s] : sMax[threadIdx.x];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        delivered[r] = sCount[0];
        totalMs[r] = sSum[0];
        maxMs[r] = sMax[0];
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct SegmentStatsGpu::Buffers {
    unsigned* order = nullptr;
    unsigned* deliveryMs = nullptr;
    unsigned* offsets = nullptr;
    unsigned* delivered = nullptr;
    unsigned long long* totalMs = nullptr;
    unsigned* maxMs = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

SegmentStatsGpu::SegmentStatsGpu(std::size_t capacity, std::size_t maxRestaurants)
    : _d(std::make_unique<Buffers>()), _capacity(capacity), _maxRestaurants(maxRestaurants)
{
    const std::size_t n = std::max<std::size_t>(capacity, 1), r = std::max<std::size_t>(maxRestaurants, 1);
    ensure(cudaMalloc(&_d->order, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->deliveryMs, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->offsets, (r + 1) * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->delivered, r * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->totalMs, r * sizeof(unsigned long long)), "cudaMalloc");
    ensure(cudaMalloc(&_d->maxMs, r * sizeof(unsigned)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

SegmentStatsGpu::~SegmentStatsGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->order);
    cudaFree(_d->deliveryMs);
    cudaFree(_d->offsets);
    cudaFree(_d->delivered);
    cudaFree(_d->totalMs);
    cudaFree(_d->maxMs);
}

std::vector<RestaurantStats> SegmentStatsGpu::stats(const Grouping& groups, const std::vector<std::uint32_t>& deliveryMs,
                                                    Timing* timing)
{
    const std::size_t restaurants = groups.offsets.size() - 1;
    if (deliveryMs.size() > _capacity || groups.order.size() > _capacity || restaurants > _maxRestaurants) {
        throw std::runtime_error("GPU: más pedidos o restaurantes que la capacidad reservada");
    }
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->order, groups.order.data(), groups.order.size() * sizeof(unsigned), cudaMemcpyHostToDevice),
           "copia de ida");
    ensure(cudaMemcpy(_d->deliveryMs, deliveryMs.data(), deliveryMs.size() * sizeof(unsigned), cudaMemcpyHostToDevice),
           "copia de ida");
    ensure(cudaMemcpy(_d->offsets, groups.offsets.data(), groups.offsets.size() * sizeof(unsigned),
                      cudaMemcpyHostToDevice),
           "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    launch(segmentStats, dim3(static_cast<unsigned>(restaurants)), dim3(kThreads),
           static_cast<const unsigned*>(_d->order), static_cast<const unsigned*>(_d->deliveryMs),
           static_cast<const unsigned*>(_d->offsets), _d->delivered, _d->totalMs, _d->maxMs);
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelBegin).count();
#endif
    std::vector<unsigned> delivered(restaurants), maxMs(restaurants);
    std::vector<unsigned long long> totalMs(restaurants);
    ensure(cudaMemcpy(delivered.data(), _d->delivered, restaurants * sizeof(unsigned), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaMemcpy(totalMs.data(), _d->totalMs, restaurants * sizeof(unsigned long long), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaMemcpy(maxMs.data(), _d->maxMs, restaurants * sizeof(unsigned), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    std::vector<RestaurantStats> result(restaurants);
    for (std::size_t r = 0; r < restaurants; ++r) {
        result[r].delivered = delivered[r];
        result[r].totalMs = totalMs[r];
        result[r].maxMs = maxMs[r];
    }
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return result;
}
