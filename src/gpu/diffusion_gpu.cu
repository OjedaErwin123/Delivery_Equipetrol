// EC2, S2: la difusión de la demanda en la GPU (ver diffusion_gpu.hpp).
#include "gpu/diffusion_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "gpu/cuda_compat.h"

namespace {

constexpr int kBx = 32, kBy = 8;        // 256 hilos; un warp recorre 32 columnas seguidas (coalescido)
constexpr int kRowsPerThread = 8;       // coarsening: cada hilo, 8 celdas de su columna

__device__ float updated(float center, float north, float south, float east, float west)
{
    const float around = north + south + east + west;  // el mismo orden que la CPU
    return kKeep * (center + kSpread * (around - 4.0f * center));
}

__global__ void stepBasic(const float* from, float* to, int w, int h)
{
    const int x = blockIdx.x * kBx + threadIdx.x, y = blockIdx.y * kBy + threadIdx.y;
    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1) {
        return;
    }
    const int c = y * w + x;
    to[c] = updated(from[c], from[c - w], from[c + w], from[c + 1], from[c - 1]);
}

__global__ void stepCoarsened(const float* from, float* to, int w, int h)
{
    const int x = blockIdx.x * kBx + threadIdx.x;
    const int y0 = (blockIdx.y * kBy + threadIdx.y) * kRowsPerThread;
    if (x < 1 || x >= w - 1 || y0 >= h - 1) {
        return;
    }
    // Arriba, centro y abajo de la columna x en registros: al bajar una fila, el centro pasa a ser el de arriba y
    // el de abajo el centro; solo se lee una celda nueva de la columna por fila.
    float up = y0 >= 1 ? from[(y0 - 1) * w + x] : 0.0f;
    float mid = from[y0 * w + x];
    for (int y = y0; y < y0 + kRowsPerThread && y < h - 1; ++y) {
        const float down = from[(y + 1) * w + x];
        if (y >= 1) {
            to[y * w + x] = updated(mid, up, down, from[y * w + x + 1], from[y * w + x - 1]);
        }
        up = mid;
        mid = down;
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct DiffusionGpu::Buffers {
    float* a = nullptr;
    float* b = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

DiffusionGpu::DiffusionGpu(std::size_t maxCells) : _d(std::make_unique<Buffers>()), _maxCells(maxCells)
{
    const std::size_t n = std::max<std::size_t>(maxCells, 1);
    ensure(cudaMalloc(&_d->a, n * sizeof(float)), "cudaMalloc");
    ensure(cudaMalloc(&_d->b, n * sizeof(float)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

DiffusionGpu::~DiffusionGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->a);
    cudaFree(_d->b);
}

Grid DiffusionGpu::diffuse(const Grid& start, int steps, bool coarsened, Timing* timing)
{
    if (start.cells.size() > _maxCells) {
        throw std::runtime_error("GPU: grilla más grande que la reservada");
    }
    // Los bordes en 0, como en la CPU (en los dos buffers: nadie los escribe).
    Grid first = start;
    for (int x = 0; x < first.width; ++x) {
        first.cells[x] = 0.0f;
        first.cells[static_cast<std::size_t>(first.height - 1) * first.width + x] = 0.0f;
    }
    for (int y = 0; y < first.height; ++y) {
        first.cells[static_cast<std::size_t>(y) * first.width] = 0.0f;
        first.cells[static_cast<std::size_t>(y) * first.width + first.width - 1] = 0.0f;
    }
    const std::size_t bytes = first.cells.size() * sizeof(float);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->a, first.cells.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemcpy(_d->b, first.cells.data(), bytes, cudaMemcpyHostToDevice), "copia de ida");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    const unsigned rows = coarsened ? cdiv(static_cast<unsigned>(first.height), kRowsPerThread)
                                    : static_cast<unsigned>(first.height);
    const dim3 blocks(cdiv(static_cast<unsigned>(first.width), kBx), cdiv(rows, kBy));
    const dim3 threads(kBx, kBy);
    float* from = _d->a;
    float* to = _d->b;
    for (int s = 0; s < steps; ++s) {
        if (coarsened) {
            launch(stepCoarsened, blocks, threads, static_cast<const float*>(from), to, first.width, first.height);
        } else {
            launch(stepBasic, blocks, threads, static_cast<const float*>(from), to, first.width, first.height);
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
    Grid out{first.width, first.height, std::vector<float>(first.cells.size())};
    ensure(cudaMemcpy(out.cells.data(), from, bytes, cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    return out;
}
