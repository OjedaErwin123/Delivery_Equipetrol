// EC2, S1: los barridos de Jacobi del mapa de cobertura en la GPU (ver coverage_gpu.hpp).
#include "gpu/coverage_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "gpu/cuda_compat.h"

namespace {

constexpr int kBx = 32, kBy = 8;  // 256 hilos: un warp por fila del bloque (lecturas coalescidas por fila)

__device__ unsigned min4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    const unsigned ab = a < b ? a : b, cd = c < d ? c : d;
    return ab < cd ? ab : cd;
}

__global__ void sweepBasic(const unsigned* from, unsigned* to, const unsigned* cost, int w, int h)
{
    const int x = blockIdx.x * kBx + threadIdx.x, y = blockIdx.y * kBy + threadIdx.y;
    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) {
        return;  // los bordes no se actualizan (están iguales en los dos buffers)
    }
    const int c = y * w + x;
    const unsigned best = min4(from[c - 1], from[c + 1], from[c - w], from[c + w]);
    const unsigned through = best + cost[c];
    to[c] = through < from[c] ? through : from[c];
}

__global__ void sweepTiled(const unsigned* from, unsigned* to, const unsigned* cost, int w, int h)
{
    __shared__ unsigned tile[kBy + 2][kBx + 2];
    const int ox = blockIdx.x * kBx - 1, oy = blockIdx.y * kBy - 1;  // esquina del bloque con el halo
    for (int i = threadIdx.y * kBx + threadIdx.x; i < (kBx + 2) * (kBy + 2); i += kBx * kBy) {
        const int sx = ox + i % (kBx + 2), sy = oy + i / (kBx + 2);
        tile[i / (kBx + 2)][i % (kBx + 2)] = (sx >= 0 && sx < w && sy >= 0 && sy < h) ? from[sy * w + sx] : kFar;
    }
    __syncthreads();
    const int x = blockIdx.x * kBx + threadIdx.x, y = blockIdx.y * kBy + threadIdx.y;
    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) {
        return;
    }
    const int tx = threadIdx.x + 1, ty = threadIdx.y + 1;
    const unsigned best = min4(tile[ty][tx - 1], tile[ty][tx + 1], tile[ty - 1][tx], tile[ty + 1][tx]);
    const unsigned through = best + cost[y * w + x];
    to[y * w + x] = through < tile[ty][tx] ? through : tile[ty][tx];
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct CoverageGpu::Buffers {
    unsigned* a = nullptr;
    unsigned* b = nullptr;
    unsigned* cost = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

CoverageGpu::CoverageGpu(std::size_t maxCells) : _d(std::make_unique<Buffers>()), _maxCells(maxCells)
{
    const std::size_t n = std::max<std::size_t>(maxCells, 1);
    ensure(cudaMalloc(&_d->a, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->b, n * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->cost, n * sizeof(unsigned)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

CoverageGpu::~CoverageGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->a);
    cudaFree(_d->b);
    cudaFree(_d->cost);
}

std::vector<std::uint32_t> CoverageGpu::relax(const CoverageGrid& grid, int sweeps, bool tiled, Timing* timing)
{
    const std::size_t cells = grid.distance.size();
    if (cells > _maxCells) {
        throw std::runtime_error("GPU: grilla más grande que la reservada");
    }
    const std::size_t bytes = cells * sizeof(unsigned);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->a, grid.distance.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemcpy(_d->b, grid.distance.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");  // los bordes
    ensure(cudaMemcpy(_d->cost, grid.cost.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    const dim3 blocks(cdiv(static_cast<unsigned>(grid.width), kBx), cdiv(static_cast<unsigned>(grid.height), kBy));
    const dim3 threads(kBx, kBy);
    unsigned* from = _d->a;
    unsigned* to = _d->b;
    for (int s = 0; s < sweeps; ++s) {
        if (tiled) {
            launch(sweepTiled, blocks, threads, static_cast<const unsigned*>(from), to,
                   static_cast<const unsigned*>(_d->cost), grid.width, grid.height);
        } else {
            launch(sweepBasic, blocks, threads, static_cast<const unsigned*>(from), to,
                   static_cast<const unsigned*>(_d->cost), grid.width, grid.height);
        }
        std::swap(from, to);
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
    std::vector<std::uint32_t> result(cells);
    ensure(cudaMemcpy(result.data(), from, bytes, cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return result;
}
