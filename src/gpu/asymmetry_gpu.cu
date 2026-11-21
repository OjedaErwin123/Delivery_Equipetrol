// EC2, P2: el rodeo de los sentidos únicos en la GPU (ver asymmetry_gpu.hpp).
#include "gpu/asymmetry_gpu.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kTile = 32;  // = DistanceTable::kTile: las filas de la tabla son múltiplo de 32
constexpr unsigned kRows = 8;   // 32 x 8 = 256 hilos; cada hilo, 4 filas del bloque
constexpr unsigned kThreads = kTile * kRows;
constexpr unsigned kUnreachable = DistanceTable::kUnreachable;

struct Partial {
    unsigned long long pairs = 0, oneWay = 0, asymmetric = 0, sumDiff = 0, worst = 0;
};

// Un par i < j (igual que comparePair de asymmetry.cpp).
__device__ void comparePair(unsigned go, unsigned back, unsigned i, unsigned j, Partial& acc)
{
    const bool there = go < kUnreachable, home = back < kUnreachable;
    if (there != home) {
        ++acc.oneWay;
    } else if (there) {
        ++acc.pairs;
        if (go != back) {
            const unsigned diff = go > back ? go - back : back - go;
            ++acc.asymmetric;
            acc.sumDiff += diff;
            const unsigned long long key =
                (static_cast<unsigned long long>(diff) << 32) | (0xffffffffu - (i << 16 | j));
            acc.worst = key > acc.worst ? key : acc.worst;
        }
    }
}

// Reduce en el bloque los cinco valores de cada hilo (sumas y un máximo) y el hilo 0 los suma al total.
__device__ void addBlock(const Partial& own, unsigned long long* totals)
{
    __shared__ unsigned long long s[5][kThreads];
    const unsigned t = threadIdx.y * kTile + threadIdx.x;
    s[0][t] = own.pairs;
    s[1][t] = own.oneWay;
    s[2][t] = own.asymmetric;
    s[3][t] = own.sumDiff;
    s[4][t] = own.worst;
    __syncthreads();
    for (unsigned half = kThreads / 2; half > 0; half >>= 1) {
        if (t < half) {
            for (int k = 0; k < 4; ++k) {
                s[k][t] += s[k][t + half];
            }
            s[4][t] = s[4][t + half] > s[4][t] ? s[4][t + half] : s[4][t];
        }
        __syncthreads();
    }
    if (t == 0) {
        for (int k = 0; k < 4; ++k) {
            atomicAdd(&totals[k], s[k][0]);
        }
        atomicMax(&totals[4], s[4][0]);
    }
}

// La vuelta directo de la memoria global: d[j][i] con j distinto en cada hilo del warp (no coalescido).
__global__ void compareNaive(const unsigned* d, unsigned stride, unsigned n, unsigned long long* totals)
{
    const unsigned I = blockIdx.y, J = blockIdx.x;
    if (I > J) {
        return;  // todo el bloque sale junto: nadie llega a la barrera de addBlock
    }
    Partial own;
    const unsigned j = J * kTile + threadIdx.x;
    for (unsigned r = threadIdx.y; r < kTile; r += kRows) {
        const unsigned i = I * kTile + r;
        if (i < j && j < n) {
            comparePair(d[i * stride + j], d[j * stride + i], i, j, own);
        }
    }
    addBlock(own, totals);
}

// Corner turning: el espejo (J, I) se copia por filas (coalescido) a memoria compartida y se lee transpuesto. Con
// Pad = 0, la columna de [32][32] está toda en el mismo banco; con Pad = 1, en 32 bancos distintos. Un cuerpo para los
// dos kernels (compareTiled y comparePadded): con nombres distintos, nsys y ncu los separan.
template <unsigned Pad>
__device__ void cornerTurning(const unsigned* d, unsigned stride, unsigned n, unsigned long long* totals)
{
    const unsigned I = blockIdx.y, J = blockIdx.x;
    if (I > J) {
        return;
    }
    __shared__ unsigned mirror[kTile][kTile + Pad];
    for (unsigned r = threadIdx.y; r < kTile; r += kRows) {
        mirror[r][threadIdx.x] = d[(J * kTile + r) * stride + I * kTile + threadIdx.x];  // fila r del espejo
    }
    __syncthreads();
    Partial own;
    const unsigned j = J * kTile + threadIdx.x;
    for (unsigned r = threadIdx.y; r < kTile; r += kRows) {
        const unsigned i = I * kTile + r;
        if (i < j && j < n) {
            comparePair(d[i * stride + j], mirror[threadIdx.x][r], i, j, own);  // mirror[x][r] = d[j][i]
        }
    }
    addBlock(own, totals);
}

__global__ void compareTiled(const unsigned* d, unsigned stride, unsigned n, unsigned long long* totals)
{
    cornerTurning<0>(d, stride, n, totals);
}

__global__ void comparePadded(const unsigned* d, unsigned stride, unsigned n, unsigned long long* totals)
{
    cornerTurning<1>(d, stride, n, totals);
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct AsymmetryGpu::Buffers {
    unsigned* table = nullptr;
    unsigned long long* totals = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

AsymmetryGpu::AsymmetryGpu(std::size_t stride) : _d(std::make_unique<Buffers>()), _stride(stride)
{
    const std::size_t cells = stride == 0 ? 1 : stride * stride;
    ensure(cudaMalloc(&_d->table, cells * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->totals, 5 * sizeof(unsigned long long)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

AsymmetryGpu::~AsymmetryGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->table);
    cudaFree(_d->totals);
}

Asymmetry AsymmetryGpu::compare(const DistanceTable& table, Kernel kernel, Timing* timing)
{
    if (table.stride > _stride || table.stride % kTile != 0 || table.n > kMaxAsymmetryNodes) {
        throw std::runtime_error("GPU: tabla más grande que la reservada o mal alineada");
    }
    const std::size_t bytes = table.stride * table.stride * sizeof(unsigned);
    const auto begin = std::chrono::steady_clock::now();
    if (bytes > 0) {
        ensure(cudaMemcpy(_d->table, table.meters.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
    }
    ensure(cudaMemset(_d->totals, 0, 5 * sizeof(unsigned long long)), "cudaMemset");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    const unsigned tiles = static_cast<unsigned>(table.stride / kTile);
    if (tiles > 0) {
        const dim3 blocks(tiles, tiles), threads(kTile, kRows);
        const unsigned* d = _d->table;
        const unsigned stride = static_cast<unsigned>(table.stride), n = static_cast<unsigned>(table.n);
        switch (kernel) {
        case Kernel::naive:
            launch(compareNaive, blocks, threads, d, stride, n, _d->totals);
            break;
        case Kernel::tiled:
            launch(compareTiled, blocks, threads, d, stride, n, _d->totals);
            break;
        case Kernel::padded:
            launch(comparePadded, blocks, threads, d, stride, n, _d->totals);
            break;
        }
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
    unsigned long long host[5];
    ensure(cudaMemcpy(host, _d->totals, sizeof(host), cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    Asymmetry a;
    a.pairs = host[0];
    a.oneWay = host[1];
    a.asymmetric = host[2];
    a.sumDiff = host[3];
    a.worst = host[4];
    return a;
}
