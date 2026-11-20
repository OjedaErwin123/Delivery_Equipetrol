// EC2, T1: Floyd–Warshall por bloques en la GPU (ver apsp_gpu.hpp). Un bloque de hilos de 32 × 32 por bloque de la
// tabla: el hilo (ty, tx) es la celda (fila ty, columna tx) del bloque.
//
// En las fases 1 y 2 el bloque propio se actualiza en memoria compartida paso a paso (k = 0..31) y lo leen otros
// hilos del mismo bloque: cada paso calcula en un registro, sincroniza, escribe y vuelve a sincronizar, así ningún
// hilo lee una celda mientras otro la escribe (compute-sanitizer --tool racecheck lo comprueba). En la fase 3 los
// dos bloques compartidos son de solo lectura: una sola sincronización después de cargarlos.
#include "gpu/apsp_gpu.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kTile = DistanceTable::kTile;  // 32: un warp por fila del bloque, 1024 hilos por bloque

__device__ unsigned minPlus(unsigned current, unsigned a, unsigned b)
{
    const unsigned through = a + b;  // a, b <= kUnreachable: la suma entra en 32 bits
    return through < current ? through : current;
}

// Fase 1: el bloque diagonal (kb, kb).
__global__ void diagonalPhase(unsigned* d, unsigned stride, unsigned kb)
{
    __shared__ unsigned own[kTile][kTile];
    const unsigned ty = threadIdx.y, tx = threadIdx.x;
    const unsigned row = kb * kTile + ty, col = kb * kTile + tx;
    own[ty][tx] = d[row * stride + col];
    __syncthreads();
    for (unsigned k = 0; k < kTile; ++k) {
        const unsigned value = minPlus(own[ty][tx], own[ty][k], own[k][tx]);
        __syncthreads();
        own[ty][tx] = value;
        __syncthreads();
    }
    d[row * stride + col] = own[ty][tx];
}

// Fase 2: blockIdx.y = 0 → el bloque (kb, t) de la fila kb; = 1 → el bloque (t, kb) de la columna kb.
// blockIdx.x recorre t salteando kb.
__global__ void crossPhase(unsigned* d, unsigned stride, unsigned kb)
{
    __shared__ unsigned pivot[kTile][kTile];
    __shared__ unsigned own[kTile][kTile];
    const unsigned t = blockIdx.x < kb ? blockIdx.x : blockIdx.x + 1;
    const bool inRow = blockIdx.y == 0;
    const unsigned ty = threadIdx.y, tx = threadIdx.x;
    const unsigned row = (inRow ? kb : t) * kTile + ty, col = (inRow ? t : kb) * kTile + tx;
    pivot[ty][tx] = d[(kb * kTile + ty) * stride + kb * kTile + tx];
    own[ty][tx] = d[row * stride + col];
    __syncthreads();
    for (unsigned k = 0; k < kTile; ++k) {
        // fila kb: d[i][j] por el pivote i→k y el propio k→j; columna kb: el propio i→k y el pivote k→j.
        const unsigned value = inRow ? minPlus(own[ty][tx], pivot[ty][k], own[k][tx])
                                     : minPlus(own[ty][tx], own[ty][k], pivot[k][tx]);
        __syncthreads();
        own[ty][tx] = value;
        __syncthreads();
    }
    d[row * stride + col] = own[ty][tx];
}

// Fase 3: el bloque (bi, bj), con bi y bj distintos de kb (blockIdx.y = bi, blockIdx.x = bj, salteando kb).
__global__ void restPhase(unsigned* d, unsigned stride, unsigned kb)
{
    __shared__ unsigned rowBlock[kTile][kTile];  // (bi, kb)
    __shared__ unsigned colBlock[kTile][kTile];  // (kb, bj)
    const unsigned bi = blockIdx.y < kb ? blockIdx.y : blockIdx.y + 1;
    const unsigned bj = blockIdx.x < kb ? blockIdx.x : blockIdx.x + 1;
    const unsigned ty = threadIdx.y, tx = threadIdx.x;
    const unsigned row = bi * kTile + ty, col = bj * kTile + tx;
    rowBlock[ty][tx] = d[row * stride + kb * kTile + tx];
    colBlock[ty][tx] = d[(kb * kTile + ty) * stride + col];
    __syncthreads();
    unsigned value = d[row * stride + col];
    for (unsigned k = 0; k < kTile; ++k) {
        value = minPlus(value, rowBlock[ty][k], colBlock[k][tx]);
    }
    d[row * stride + col] = value;
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct ApspGpu::Buffers {
    unsigned* table = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

ApspGpu::ApspGpu(std::size_t maxStride) : _d(std::make_unique<Buffers>()), _maxStride(maxStride)
{
    ensure(cudaMalloc(&_d->table, maxStride * maxStride * sizeof(unsigned)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

ApspGpu::~ApspGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->table);
}

void ApspGpu::solve(DistanceTable& table, Timing* timing)
{
    if (table.stride > _maxStride || table.stride % kTile != 0) {
        throw std::runtime_error("GPU: tabla más grande que la reservada o sin relleno a múltiplo de 32");
    }
    const unsigned stride = static_cast<unsigned>(table.stride);
    const unsigned blocks = stride / kTile;
    const std::size_t bytes = table.meters.size() * sizeof(unsigned);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->table, table.meters.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelsBegin = std::chrono::steady_clock::now();
#endif
    for (unsigned kb = 0; kb < blocks; ++kb) {
        launch(diagonalPhase, dim3(1), dim3(kTile, kTile), _d->table, stride, kb);
        if (blocks > 1) {
            launch(crossPhase, dim3(blocks - 1, 2), dim3(kTile, kTile), _d->table, stride, kb);
            launch(restPhase, dim3(blocks - 1, blocks - 1), dim3(kTile, kTile), _d->table, stride, kb);
        }
    }
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelsBegin).count();
#endif
    ensure(cudaMemcpy(table.meters.data(), _d->table, bytes, cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
}
