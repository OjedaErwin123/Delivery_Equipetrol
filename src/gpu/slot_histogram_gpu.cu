// EC2, H2: el histograma 2D (restaurante, franja) en la GPU (ver slot_histogram_gpu.hpp).
#include "gpu/slot_histogram_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kThreads = 256;
constexpr unsigned kMaxBlocks = 160;  // 4 bloques por SM en la T4: con más pedidos, cada hilo cuenta varios
constexpr unsigned kBins = static_cast<unsigned>(kMaxSlotRestaurants * kSlots);  // 9216 cajas como máximo

__device__ unsigned slotOnDevice(unsigned createdMs)
{
    const unsigned s = createdMs / kSlotMs;
    return s < kSlots ? s : static_cast<unsigned>(kSlots) - 1;
}

// `interleaved`: el recorrido intercalado (coalescido) o el contiguo (no coalescido). bins = restaurantes x 144.
__global__ void slotHistogram(const unsigned* restaurantOf, const unsigned* createdMs, unsigned n, unsigned bins,
                              bool interleaved, unsigned long long* counts)
{
    __shared__ unsigned own[kBins];
    for (unsigned b = threadIdx.x; b < bins; b += blockDim.x) {
        own[b] = 0;
    }
    __syncthreads();
    const unsigned threads = gridDim.x * blockDim.x;
    const unsigned me = blockIdx.x * blockDim.x + threadIdx.x;
    if (interleaved) {
        for (unsigned i = me; i < n; i += threads) {
            atomicAdd(&own[restaurantOf[i] * static_cast<unsigned>(kSlots) + slotOnDevice(createdMs[i])], 1u);
        }
    } else {
        const unsigned long long begin = static_cast<unsigned long long>(n) * me / threads;
        const unsigned long long end = static_cast<unsigned long long>(n) * (me + 1) / threads;
        for (unsigned long long i = begin; i < end; ++i) {
            atomicAdd(&own[restaurantOf[i] * static_cast<unsigned>(kSlots) + slotOnDevice(createdMs[i])], 1u);
        }
    }
    __syncthreads();
    for (unsigned b = threadIdx.x; b < bins; b += blockDim.x) {
        if (own[b] != 0) {
            atomicAdd(&counts[b], static_cast<unsigned long long>(own[b]));
        }
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct SlotHistogramGpu::Buffers {
    unsigned* restaurantOf = nullptr;
    unsigned* createdMs = nullptr;
    unsigned long long* counts = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

SlotHistogramGpu::SlotHistogramGpu(std::size_t capacity) : _d(std::make_unique<Buffers>()), _capacity(capacity)
{
    const std::size_t n = std::max<std::size_t>(capacity, 1);
    ensure(cudaMalloc(&_d->restaurantOf, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->createdMs, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->counts, kBins * sizeof(unsigned long long)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

SlotHistogramGpu::~SlotHistogramGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->restaurantOf);
    cudaFree(_d->createdMs);
    cudaFree(_d->counts);
}

unsigned SlotHistogramGpu::blocksFor(std::size_t n)
{
    return std::max(1u, std::min(cdiv(static_cast<unsigned>(n), kThreads), kMaxBlocks));
}

SlotHistogram SlotHistogramGpu::count(const std::vector<std::uint32_t>& restaurantOf,
                                      const std::vector<std::uint32_t>& createdMs, std::size_t restaurants,
                                      bool interleaved, Timing* timing)
{
    if (restaurantOf.size() > _capacity || restaurants > kMaxSlotRestaurants) {
        throw std::runtime_error("GPU: más pedidos que la capacidad reservada o más de 64 restaurantes");
    }
    const unsigned n = static_cast<unsigned>(restaurantOf.size());
    const unsigned bins = static_cast<unsigned>(restaurants * kSlots);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->restaurantOf, restaurantOf.data(), n * sizeof(unsigned), cudaMemcpyHostToDevice),
           "copia de ida");
    ensure(cudaMemcpy(_d->createdMs, createdMs.data(), n * sizeof(unsigned), cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemset(_d->counts, 0, bins * sizeof(unsigned long long)), "cudaMemset");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    launch(slotHistogram, dim3(blocksFor(n)), dim3(kThreads), static_cast<const unsigned*>(_d->restaurantOf),
           static_cast<const unsigned*>(_d->createdMs), n, bins, interleaved, _d->counts);
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelBegin).count();
#endif
    std::vector<unsigned long long> counts(bins);
    ensure(cudaMemcpy(counts.data(), _d->counts, bins * sizeof(unsigned long long), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return SlotHistogram(counts.begin(), counts.end());
}
