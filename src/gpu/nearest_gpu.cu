// EC2, R1: el prefiltro de las motos más cercanas en la GPU (ver nearest_gpu.hpp). Tres kernels:
//  1. distancesAndBlockTop: un hilo por moto calcula su distancia (con un bucle de paso de grid si hay más motos
//     que hilos) y cada bloque hace `rounds` reducciones argmin seguidas en memoria compartida: en la ronda r
//     encuentra la menor clave (distancia, posición) estrictamente mayor que la de la ronda r-1. Así cada bloque
//     deja sus `rounds` mejores, en orden. La clave es única (la posición desempata): el resultado no depende del
//     orden en que se combinan los pares.
//  2. mergeTop: un solo bloque repite lo mismo sobre las mejores de todos los bloques y deja el umbral: la
//     rounds-ésima clave + 2 · kNearestToleranceM.
//  3. collect: un hilo por moto; las que no superan el umbral se anotan con atomicAdd (el orden de salida no es
//     determinista: el host las ordena).
#include "gpu/nearest_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "gpu/cuda_compat.h"
#include "nearest.hpp"

namespace {

constexpr unsigned kThreads = 256;   // hilos por bloque: 8 warps, una potencia de 2 para la reducción en árbol
constexpr unsigned kMaxBlocks = 160; // 4 bloques por SM en la T4 (40 SM): con más motos, cada hilo hace varias
constexpr unsigned kMaxRounds = 64;  // limit + 1 como máximo (el despachador usa 9)
constexpr double kFar = 1e300;       // "ninguna": mayor que cualquier distancia
constexpr unsigned kNone = 0xffffffffu;

// ¿(d1, p1) va antes que (d2, p2)? El orden de las claves.
__host__ __device__ inline bool before(double d1, unsigned p1, double d2, unsigned p2)
{
    return d1 < d2 || (d1 == d2 && p1 < p2);
}

// La misma fórmula y el mismo orden de operaciones que haversineMeters (road_graph.cpp).
__device__ double radiansOnDevice(double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}

__device__ double haversineOnDevice(double lat1, double lon1, double lat2, double lon2)
{
    const double dLat = radiansOnDevice(lat2 - lat1);
    const double dLon = radiansOnDevice(lon2 - lon1);
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(radiansOnDevice(lat1)) * cos(radiansOnDevice(lat2)) * sin(dLon / 2) * sin(dLon / 2);
    return 2 * 6371000.0 * asin(sqrt(a));
}

// Reducción argmin del bloque sobre (sDist, sPos), en árbol: al terminar, la menor queda en la posición 0.
__device__ void blockArgmin(double* sDist, unsigned* sPos)
{
    for (unsigned s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s && before(sDist[threadIdx.x + s], sPos[threadIdx.x + s], sDist[threadIdx.x], sPos[threadIdx.x])) {
            sDist[threadIdx.x] = sDist[threadIdx.x + s];
            sPos[threadIdx.x] = sPos[threadIdx.x + s];
        }
        __syncthreads();
    }
}

__global__ void distancesAndBlockTop(const Coordinates* at, unsigned n, double shopLat, double shopLon, double* dist,
                                     unsigned rounds, double* topDist, unsigned* topPos)
{
    __shared__ double sDist[kThreads];
    __shared__ unsigned sPos[kThreads];
    __shared__ double prevDist;
    __shared__ unsigned prevPos;
    const unsigned stride = gridDim.x * blockDim.x;
    for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
        dist[i] = haversineOnDevice(at[i].lat, at[i].lon, shopLat, shopLon);
    }
    if (threadIdx.x == 0) {
        prevDist = -1.0;  // antes que cualquier distancia
        prevPos = 0;
    }
    __syncthreads();
    for (unsigned r = 0; r < rounds; ++r) {
        double bestDist = kFar;
        unsigned bestPos = kNone;
        for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {  // las mismas que escribió
            const double d = dist[i];
            if (before(prevDist, prevPos, d, i) && before(d, i, bestDist, bestPos)) {
                bestDist = d;
                bestPos = i;
            }
        }
        sDist[threadIdx.x] = bestDist;
        sPos[threadIdx.x] = bestPos;
        __syncthreads();
        blockArgmin(sDist, sPos);
        if (threadIdx.x == 0) {
            topDist[blockIdx.x * rounds + r] = sDist[0];
            topPos[blockIdx.x * rounds + r] = sPos[0];
            prevDist = sDist[0];
            prevPos = sPos[0];
        }
        __syncthreads();
    }
}

__global__ void mergeTop(const double* topDist, const unsigned* topPos, unsigned count, unsigned rounds,
                         double tolerance, double* threshold)
{
    __shared__ double sDist[kThreads];
    __shared__ unsigned sPos[kThreads];
    __shared__ double prevDist;
    __shared__ unsigned prevPos;
    if (threadIdx.x == 0) {
        prevDist = -1.0;
        prevPos = 0;
    }
    __syncthreads();
    for (unsigned r = 0; r < rounds; ++r) {
        double bestDist = kFar;
        unsigned bestPos = kNone;
        for (unsigned j = threadIdx.x; j < count; j += blockDim.x) {
            const double d = topDist[j];
            const unsigned p = topPos[j];
            if (p != kNone && before(prevDist, prevPos, d, p) && before(d, p, bestDist, bestPos)) {
                bestDist = d;
                bestPos = p;
            }
        }
        sDist[threadIdx.x] = bestDist;
        sPos[threadIdx.x] = bestPos;
        __syncthreads();
        blockArgmin(sDist, sPos);
        if (threadIdx.x == 0) {
            prevDist = sDist[0];
            prevPos = sPos[0];
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        *threshold = prevDist + 2 * tolerance;
    }
}

__global__ void collect(const double* dist, unsigned n, const double* threshold, unsigned* count, unsigned* out)
{
    const unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n && dist[i] <= *threshold) {
        out[atomicAdd(count, 1u)] = i;
    }
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

}  // namespace

struct NearestGpu::Buffers {
    Coordinates* at = nullptr;
    double* dist = nullptr;
    double* topDist = nullptr;
    unsigned* topPos = nullptr;
    double* threshold = nullptr;
    unsigned* count = nullptr;
    unsigned* out = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

NearestGpu::NearestGpu(std::size_t capacity) : _d(std::make_unique<Buffers>()), _capacity(capacity)
{
    ensure(cudaMalloc(&_d->at, capacity * sizeof(Coordinates)), "cudaMalloc");
    ensure(cudaMalloc(&_d->dist, capacity * sizeof(double)), "cudaMalloc");
    ensure(cudaMalloc(&_d->topDist, kMaxBlocks * kMaxRounds * sizeof(double)), "cudaMalloc");
    ensure(cudaMalloc(&_d->topPos, kMaxBlocks * kMaxRounds * sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->threshold, sizeof(double)), "cudaMalloc");
    ensure(cudaMalloc(&_d->count, sizeof(unsigned)), "cudaMalloc");
    ensure(cudaMalloc(&_d->out, capacity * sizeof(unsigned)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

NearestGpu::~NearestGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->at);
    cudaFree(_d->dist);
    cudaFree(_d->topDist);
    cudaFree(_d->topPos);
    cudaFree(_d->threshold);
    cudaFree(_d->count);
    cudaFree(_d->out);
}

unsigned NearestGpu::blocksFor(std::size_t n)
{
    const unsigned blocks = cdiv(static_cast<unsigned>(n), kThreads);
    return std::max(1u, std::min(blocks, kMaxBlocks));
}

std::string NearestGpu::deviceName()
{
    cudaDeviceProp properties;
    ensure(cudaGetDeviceProperties(&properties, 0), "cudaGetDeviceProperties");
    return properties.name;
}

std::vector<std::size_t> NearestGpu::superset(const std::vector<Coordinates>& at, Coordinates shop, std::size_t limit,
                                              Timing* timing)
{
    std::vector<std::size_t> all(at.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    if (limit == 0 || at.size() <= limit) {
        return all;
    }
    if (at.size() > _capacity || limit + 1 > kMaxRounds) {
        throw std::runtime_error("GPU: más motos que la capacidad reservada o límite demasiado grande");
    }
    const unsigned n = static_cast<unsigned>(at.size());
    const unsigned rounds = static_cast<unsigned>(limit + 1);
    const unsigned blocks = blocksFor(n);
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->at, at.data(), n * sizeof(Coordinates), cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemset(_d->count, 0, sizeof(unsigned)), "cudaMemset");
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->start), "cudaEventRecord");
#else
    const auto kernelsBegin = std::chrono::steady_clock::now();
#endif
    launch(distancesAndBlockTop, dim3(blocks), dim3(kThreads), static_cast<const Coordinates*>(_d->at), n, shop.lat,
           shop.lon, _d->dist, rounds, _d->topDist, _d->topPos);
    launch(mergeTop, dim3(1), dim3(kThreads), static_cast<const double*>(_d->topDist),
           static_cast<const unsigned*>(_d->topPos), blocks * rounds, rounds, kNearestToleranceM, _d->threshold);
    launch(collect, dim3(cdiv(n, kThreads)), dim3(kThreads), static_cast<const double*>(_d->dist), n,
           static_cast<const double*>(_d->threshold), _d->count, _d->out);
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(_d->stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(_d->stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, _d->start, _d->stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelsBegin).count();
#endif
    unsigned count = 0;
    ensure(cudaMemcpy(&count, _d->count, sizeof(unsigned), cudaMemcpyDeviceToHost), "copia de vuelta");
    std::vector<unsigned> chosen(count);
    ensure(cudaMemcpy(chosen.data(), _d->out, count * sizeof(unsigned), cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    const auto end = std::chrono::steady_clock::now();
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs = std::chrono::duration<double, std::milli>(end - begin).count();
    }
    std::sort(chosen.begin(), chosen.end());  // atomicAdd las anota en cualquier orden
    return std::vector<std::size_t>(chosen.begin(), chosen.end());
}
