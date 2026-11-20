// EC2, H1: el histograma de los tiempos de entrega en la GPU (ver histogram_gpu.hpp).
#include "gpu/histogram_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kThreads = 256;    // 8 warps; también es el número de cajas: un hilo inicializa y vuelca una
constexpr unsigned kMaxBlocks = 160;  // 4 bloques por SM en la T4: con más datos, cada hilo cuenta varios (coarsening)
static_assert(kDeliveryBins == kThreads, "un hilo por caja al inicializar y volcar el histograma del bloque");

__device__ unsigned binOnDevice(unsigned ms)
{
    const unsigned bin = ms / kBinMs;
    return bin < kDeliveryBins ? bin : static_cast<unsigned>(kDeliveryBins) - 1;
}

// Ingenuo: un atómico global por entrega.
__global__ void histogramGlobal(const unsigned* ms, unsigned n, unsigned long long* counts)
{
    const unsigned stride = gridDim.x * blockDim.x;
    for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        atomicAdd(&counts[binOnDevice(ms[i])], 1ull);
    }
}

// Privatizado: el histograma del bloque en memoria compartida; se vuelca al global una vez al final.
__global__ void histogramPrivatized(const unsigned* ms, unsigned n, unsigned long long* counts)
{
    __shared__ unsigned own[kDeliveryBins];  // un bloque cuenta a lo sumo n < 2^32 entregas: 32 bits alcanzan
    own[threadIdx.x] = 0;
    __syncthreads();
    const unsigned stride = gridDim.x * blockDim.x;
    for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        atomicAdd(&own[binOnDevice(ms[i])], 1u);
    }
    __syncthreads();
    if (own[threadIdx.x] != 0) {
        atomicAdd(&counts[threadIdx.x], static_cast<unsigned long long>(own[threadIdx.x]));
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct HistogramGpu::Buffers {
    unsigned* ms = nullptr;
    unsigned long long* counts = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

HistogramGpu::HistogramGpu(std::size_t capacity) : _d(std::make_unique<Buffers>()), _capacity(capacity)
{
    ensure(cudaMalloc(&_d->ms, std::max<std::size_t>(capacity, 1) * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->counts, kDeliveryBins * sizeof(unsigned long long)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

HistogramGpu::~HistogramGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->ms);
    cudaFree(_d->counts);
}

unsigned HistogramGpu::blocksFor(std::size_t n)
{
    return std::max(1u, std::min(cdiv(static_cast<unsigned>(n), kThreads), kMaxBlocks));
}

Histogram HistogramGpu::privatized(const std::vector<std::uint32_t>& ms, Timing* timing)
{
    return run(ms, true, timing);
}

Histogram HistogramGpu::globalAtomics(const std::vector<std::uint32_t>& ms, Timing* timing)
{
    return run(ms, false, timing);
}

Histogram HistogramGpu::run(const std::vector<std::uint32_t>& ms, bool privatize, Timing* timing)
{
    if (ms.size() > _capacity) {
        throw std::runtime_error("GPU: más tiempos de entrega que la capacidad reservada");
    }
    const unsigned n = static_cast<unsigned>(ms.size());
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->ms, ms.data(), n * sizeof(unsigned), cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemset(_d->counts, 0, kDeliveryBins * sizeof(unsigned long long)), "cudaMemset");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    if (privatize) {
        launch(histogramPrivatized, dim3(blocksFor(n)), dim3(kThreads), static_cast<const unsigned*>(_d->ms), n,
               _d->counts);
    } else {
        launch(histogramGlobal, dim3(blocksFor(n)), dim3(kThreads), static_cast<const unsigned*>(_d->ms), n,
               _d->counts);
    }
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelBegin).count();
#endif
    std::vector<unsigned long long> counts(kDeliveryBins);
    ensure(cudaMemcpy(counts.data(), _d->counts, kDeliveryBins * sizeof(unsigned long long), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return Histogram(counts.begin(), counts.end());
}
