// EC2, C1: la convolución gaussiana del mapa de calor en la GPU (ver heatmap_gpu.hpp).
#include "gpu/heatmap_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr int kOut = 16;                  // celdas de salida por lado de cada bloque: 256 hilos
constexpr int kIn = kOut + 2 * kRadius;   // 28: el bloque de entrada con el halo

__constant__ float cFilter[kTaps * kTaps];

__global__ void convolveBasic(const float* in, float* out, int width, int height)
{
    const int x = blockIdx.x * kOut + threadIdx.x, y = blockIdx.y * kOut + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    float sum = 0.0f;
    for (int fy = 0; fy < kTaps; ++fy) {
        const int sy = y + fy - kRadius;
        for (int fx = 0; fx < kTaps; ++fx) {
            const int sx = x + fx - kRadius;
            const float v = (sx >= 0 && sx < width && sy >= 0 && sy < height) ? in[sy * width + sx] : 0.0f;
            sum += cFilter[fy * kTaps + fx] * v;
        }
    }
    out[y * width + x] = sum;
}

__global__ void convolveTiled(const float* in, float* out, int width, int height)
{
    __shared__ float tile[kIn][kIn];
    const int ox = blockIdx.x * kOut - kRadius, oy = blockIdx.y * kOut - kRadius;  // esquina del bloque de entrada
    for (int i = threadIdx.y * kOut + threadIdx.x; i < kIn * kIn; i += kOut * kOut) {  // ~3 valores por hilo
        const int sx = ox + i % kIn, sy = oy + i / kIn;
        tile[i / kIn][i % kIn] = (sx >= 0 && sx < width && sy >= 0 && sy < height) ? in[sy * width + sx] : 0.0f;
    }
    __syncthreads();
    const int x = blockIdx.x * kOut + threadIdx.x, y = blockIdx.y * kOut + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    float sum = 0.0f;
    for (int fy = 0; fy < kTaps; ++fy) {
        for (int fx = 0; fx < kTaps; ++fx) {
            sum += cFilter[fy * kTaps + fx] * tile[threadIdx.y + fy][threadIdx.x + fx];
        }
    }
    out[y * width + x] = sum;
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct HeatmapGpu::Buffers {
    float* in = nullptr;
    float* out = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

HeatmapGpu::HeatmapGpu(std::size_t maxCells) : _d(std::make_unique<Buffers>()), _maxCells(maxCells)
{
    const std::size_t n = std::max<std::size_t>(maxCells, 1);
    ensure(cudaMalloc(&_d->in, n * sizeof(float)), "cudaMalloc");
    ensure(cudaMalloc(&_d->out, n * sizeof(float)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

HeatmapGpu::~HeatmapGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->in);
    cudaFree(_d->out);
}

Grid HeatmapGpu::convolve(const Grid& in, const std::vector<float>& filter, bool tiled, Timing* timing)
{
    if (in.cells.size() > _maxCells || filter.size() != static_cast<std::size_t>(kTaps * kTaps)) {
        throw std::runtime_error("GPU: grilla más grande que la reservada o filtro de otro tamaño");
    }
    const std::size_t bytes = in.cells.size() * sizeof(float);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpyToSymbol(cFilter, filter.data(), filter.size() * sizeof(float)), "copia del filtro");
    ensure(cudaMemcpy(_d->in, in.cells.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    const dim3 grid(cdiv(static_cast<unsigned>(in.width), kOut), cdiv(static_cast<unsigned>(in.height), kOut));
    const dim3 block(kOut, kOut);
    if (tiled) {
        launch(convolveTiled, grid, block, static_cast<const float*>(_d->in), _d->out, in.width, in.height);
    } else {
        launch(convolveBasic, grid, block, static_cast<const float*>(_d->in), _d->out, in.width, in.height);
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
    Grid out{in.width, in.height, std::vector<float>(in.cells.size())};
    ensure(cudaMemcpy(out.cells.data(), _d->out, bytes, cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return out;
}
