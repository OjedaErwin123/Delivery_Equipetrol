// EC2, SC1: el counting sort estable de los pedidos por restaurante en la GPU (ver grouping_gpu.hpp).
#include "gpu/grouping_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kThreads = 128;      // hilos por bloque en los kernels 1 y 3 (la tabla [64][128] = 32 KB)
constexpr unsigned kMaxBlocks = 160;    // 4 bloques por SM en la T4
constexpr unsigned kScanThreads = 1024; // el scan: un bloque de 1024 hilos que recorre los totales de a 1024
constexpr unsigned kRows = static_cast<unsigned>(GroupingGpu::kMaxRestaurants);

// El sub-tramo [begin, end) del hilo `t` del bloque `b`, de `blocks` bloques, sobre n pedidos.
__device__ void subRange(unsigned n, unsigned blocks, unsigned b, unsigned t, unsigned& begin, unsigned& end)
{
    const unsigned long long blockBegin = static_cast<unsigned long long>(n) * b / blocks;
    const unsigned long long blockEnd = static_cast<unsigned long long>(n) * (b + 1) / blocks;
    const unsigned long long size = blockEnd - blockBegin;
    begin = static_cast<unsigned>(blockBegin + size * t / kThreads);
    end = static_cast<unsigned>(blockBegin + size * (t + 1) / kThreads);
}

// Cuenta el sub-tramo del hilo en su columna de `table` (cada hilo escribe solo su columna: sin atómicos).
__device__ void countOwn(const unsigned* restaurantOf, unsigned n, unsigned restaurants, unsigned (*table)[kThreads])
{
    for (unsigned r = 0; r < restaurants; ++r) {
        table[r][threadIdx.x] = 0;
    }
    unsigned begin = 0, end = 0;
    subRange(n, gridDim.x, blockIdx.x, threadIdx.x, begin, end);
    for (unsigned i = begin; i < end; ++i) {
        ++table[restaurantOf[i]][threadIdx.x];
    }
}

__global__ void countBlocks(const unsigned* restaurantOf, unsigned n, unsigned restaurants, unsigned* blockCount)
{
    __shared__ unsigned table[kRows][kThreads];
    countOwn(restaurantOf, n, restaurants, table);
    __syncthreads();
    if (threadIdx.x < restaurants) {  // un hilo por restaurante suma su fila
        unsigned sum = 0;
        for (unsigned t = 0; t < kThreads; ++t) {
            sum += table[threadIdx.x][t];
        }
        blockCount[threadIdx.x * gridDim.x + blockIdx.x] = sum;  // orden (restaurante, bloque)
    }
}

// Scan exclusivo de `count` valores con un bloque: de a kScanThreads, cada pedazo con Kogge–Stone en memoria
// compartida (log2(1024) = 10 pasos; en cada paso cada hilo lee, se sincroniza y escribe) más lo acumulado antes.
__global__ void scanCounts(const unsigned* in, unsigned count, unsigned* out)
{
    __shared__ unsigned s[kScanThreads];
    __shared__ unsigned carry;
    if (threadIdx.x == 0) {
        carry = 0;
    }
    __syncthreads();
    for (unsigned base = 0; base < count; base += kScanThreads) {
        const unsigned i = base + threadIdx.x;
        const unsigned value = i < count ? in[i] : 0;
        s[threadIdx.x] = value;
        __syncthreads();
        for (unsigned offset = 1; offset < kScanThreads; offset <<= 1) {
            const unsigned before = threadIdx.x >= offset ? s[threadIdx.x - offset] : 0;
            __syncthreads();
            s[threadIdx.x] += before;
            __syncthreads();
        }
        if (i < count) {
            out[i] = carry + s[threadIdx.x] - value;  // inclusivo - propio = exclusivo
        }
        __syncthreads();
        if (threadIdx.x == kScanThreads - 1) {
            carry += s[threadIdx.x];
        }
        __syncthreads();
    }
}

__global__ void scatterStable(const unsigned* restaurantOf, unsigned n, unsigned restaurants, const unsigned* starts,
                              unsigned* order)
{
    __shared__ unsigned table[kRows][kThreads];
    countOwn(restaurantOf, n, restaurants, table);
    __syncthreads();
    if (threadIdx.x < restaurants) {  // su fila: scan exclusivo por hilos + dónde empieza este bloque en el grupo
        unsigned running = starts[threadIdx.x * gridDim.x + blockIdx.x];
        for (unsigned t = 0; t < kThreads; ++t) {
            const unsigned c = table[threadIdx.x][t];
            table[threadIdx.x][t] = running;
            running += c;
        }
    }
    __syncthreads();
    unsigned begin = 0, end = 0;
    subRange(n, gridDim.x, blockIdx.x, threadIdx.x, begin, end);
    for (unsigned i = begin; i < end; ++i) {  // en orden: estable
        order[table[restaurantOf[i]][threadIdx.x]++] = i;
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct GroupingGpu::Buffers {
    unsigned* restaurantOf = nullptr;
    unsigned* order = nullptr;
    unsigned* blockCount = nullptr;
    unsigned* starts = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

GroupingGpu::GroupingGpu(std::size_t capacity) : _d(std::make_unique<Buffers>()), _capacity(capacity)
{
    const std::size_t n = std::max<std::size_t>(capacity, 1);
    ensure(cudaMalloc(&_d->restaurantOf, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->order, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->blockCount, kRows * kMaxBlocks * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->starts, kRows * kMaxBlocks * sizeof(unsigned)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

GroupingGpu::~GroupingGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->restaurantOf);
    cudaFree(_d->order);
    cudaFree(_d->blockCount);
    cudaFree(_d->starts);
}

unsigned GroupingGpu::blocksFor(std::size_t n)
{
    // Al menos 32 pedidos por hilo (sin eso el recuento de la tabla domina) y a lo sumo 160 bloques.
    return std::max(1u, std::min(cdiv(static_cast<unsigned>(n), kThreads * 32), kMaxBlocks));
}

Grouping GroupingGpu::group(const std::vector<std::uint32_t>& restaurantOf, std::size_t restaurants, Timing* timing)
{
    if (restaurantOf.size() > _capacity || restaurants > kMaxRestaurants) {
        throw std::runtime_error("GPU: más pedidos que la capacidad reservada o más de 64 restaurantes");
    }
    const unsigned n = static_cast<unsigned>(restaurantOf.size());
    const unsigned r = static_cast<unsigned>(restaurants);
    const unsigned blocks = blocksFor(n);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->restaurantOf, restaurantOf.data(), n * sizeof(unsigned), cudaMemcpyHostToDevice),
           "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    launch(countBlocks, dim3(blocks), dim3(kThreads), static_cast<const unsigned*>(_d->restaurantOf), n, r,
           _d->blockCount);
    launch(scanCounts, dim3(1), dim3(kScanThreads), static_cast<const unsigned*>(_d->blockCount), r * blocks,
           _d->starts);
    launch(scatterStable, dim3(blocks), dim3(kThreads), static_cast<const unsigned*>(_d->restaurantOf), n, r,
           static_cast<const unsigned*>(_d->starts), _d->order);
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelBegin).count();
#endif
    Grouping g;
    g.order.resize(n);
    std::vector<unsigned> starts(static_cast<std::size_t>(r) * blocks);
    ensure(cudaMemcpy(g.order.data(), _d->order, n * sizeof(unsigned), cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaMemcpy(starts.data(), _d->starts, starts.size() * sizeof(unsigned), cudaMemcpyDeviceToHost),
           "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    g.offsets.assign(r + 1, n);
    for (unsigned row = 0; row < r; ++row) {
        g.offsets[row] = starts[static_cast<std::size_t>(row) * blocks];  // dónde empieza el bloque 0 en el grupo
    }
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return g;
}
