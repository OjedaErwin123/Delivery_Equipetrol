// EC2, P1: las fases de los pedidos en la GPU, AoS contra SoA (ver phases_gpu.hpp).
#include "gpu/phases_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

#include "gpu/cuda_compat.h"

namespace {

constexpr unsigned kThreads = 256;
constexpr unsigned kMaxBlocks = 160;  // 4 bloques por SM en la T4: con más pedidos, cada hilo suma varios

// Reduce en el bloque las cuatro sumas del hilo y el hilo 0 las suma al total (4 atómicos por bloque).
__device__ void addBlock(unsigned long long count, unsigned long long wait, unsigned long long pickup,
                         unsigned long long ride, unsigned long long* totals)
{
    __shared__ unsigned long long s[4][kThreads];
    s[0][threadIdx.x] = count;
    s[1][threadIdx.x] = wait;
    s[2][threadIdx.x] = pickup;
    s[3][threadIdx.x] = ride;
    __syncthreads();
    for (unsigned half = blockDim.x / 2; half > 0; half >>= 1) {
        if (threadIdx.x < half) {
            for (int k = 0; k < 4; ++k) {
                s[k][threadIdx.x] += s[k][threadIdx.x + half];
            }
        }
        __syncthreads();
    }
    if (threadIdx.x == 0) {
        for (int k = 0; k < 4; ++k) {
            atomicAdd(&totals[k], s[k][0]);
        }
    }
}

__global__ void phasesAoS(const OrderRecord* records, unsigned n, unsigned long long* totals)
{
    unsigned long long count = 0, wait = 0, pickup = 0, ride = 0;
    for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
        const OrderRecord r = records[i];  // trae el registro entero (32 bytes) aunque use 16
        if (r.deliveredMs != kNotYet) {
            ++count;
            wait += r.assignedMs - r.createdMs;
            pickup += r.pickedUpMs - r.assignedMs;
            ride += r.deliveredMs - r.pickedUpMs;
        }
    }
    addBlock(count, wait, pickup, ride, totals);
}

__global__ void phasesSoA(const unsigned* created, const unsigned* assigned, const unsigned* pickedUp,
                          const unsigned* delivered, unsigned n, unsigned long long* totals)
{
    unsigned long long count = 0, wait = 0, pickup = 0, ride = 0;
    for (unsigned i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
        const unsigned d = delivered[i];
        if (d != kNotYet) {
            const unsigned c = created[i], a = assigned[i], p = pickedUp[i];
            ++count;
            wait += a - c;
            pickup += p - a;
            ride += d - p;
        }
    }
    addBlock(count, wait, pickup, ride, totals);
}

void ensure(cudaError_t error, const char* what)
{
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU: ") + what + ": " + cudaGetErrorString(error));
    }
}

unsigned blocksFor(std::size_t n)
{
    return std::max(1u, std::min(cdiv(static_cast<unsigned>(n), kThreads), kMaxBlocks));
}

}  // namespace

struct PhasesGpu::Buffers {
    OrderRecord* records = nullptr;
    unsigned* columns[4] = {nullptr, nullptr, nullptr, nullptr};
    unsigned long long* totals = nullptr;
#ifdef __CUDACC__
    cudaEvent_t start{}, stop{};
#endif
};

PhasesGpu::PhasesGpu(std::size_t capacity) : _d(std::make_unique<Buffers>()), _capacity(capacity)
{
    const std::size_t n = std::max<std::size_t>(capacity, 1);
    ensure(cudaMalloc(&_d->records, n * sizeof(OrderRecord)), "cudaMalloc");
    for (unsigned*& column : _d->columns) {
        ensure(cudaMalloc(&column, n * sizeof(unsigned)), "cudaMalloc");
    }
    ensure(cudaMalloc(&_d->totals, 4 * sizeof(unsigned long long)), "cudaMalloc");
#ifdef __CUDACC__
    ensure(cudaEventCreate(&_d->start), "cudaEventCreate");
    ensure(cudaEventCreate(&_d->stop), "cudaEventCreate");
#endif
}

PhasesGpu::~PhasesGpu()
{
#ifdef __CUDACC__
    cudaEventDestroy(_d->start);
    cudaEventDestroy(_d->stop);
#endif
    cudaFree(_d->records);
    for (unsigned* column : _d->columns) {
        cudaFree(column);
    }
    cudaFree(_d->totals);
}

namespace {

// Lo común a las dos versiones: medir el kernel (que ya está encolado por `run`) y traer los totales.
template <typename Run>
PhaseTotals measured(PhasesGpu::Timing* timing, std::chrono::steady_clock::time_point begin, unsigned long long* totals,
                     void* startEvent, void* stopEvent, Run run)
{
#ifdef __CUDACC__
    cudaEvent_t start = static_cast<cudaEvent_t>(startEvent), stop = static_cast<cudaEvent_t>(stopEvent);
    ensure(cudaEventRecord(start), "cudaEventRecord");
#else
    (void)startEvent;
    (void)stopEvent;
    const auto kernelBegin = std::chrono::steady_clock::now();
#endif
    run();
    ensure(cudaGetLastError(), "lanzamiento");
    float kernelMs = 0;
#ifdef __CUDACC__
    ensure(cudaEventRecord(stop), "cudaEventRecord");
    ensure(cudaEventSynchronize(stop), "cudaEventSynchronize");
    ensure(cudaEventElapsedTime(&kernelMs, start, stop), "cudaEventElapsedTime");
#else
    kernelMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - kernelBegin).count();
#endif
    unsigned long long host[4];
    ensure(cudaMemcpy(host, totals, sizeof(host), cudaMemcpyDeviceToHost), "copia de vuelta");
    ensure(cudaDeviceSynchronize(), "cudaDeviceSynchronize");
    if (timing != nullptr) {
        timing->kernelMs = kernelMs;
        timing->endToEndMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    PhaseTotals t;
    t.delivered = host[0];
    t.waitMs = host[1];
    t.pickupMs = host[2];
    t.rideMs = host[3];
    return t;
}

}  // namespace

PhaseTotals PhasesGpu::fromRecords(const std::vector<OrderRecord>& records, Timing* timing)
{
    if (records.size() > _capacity) {
        throw std::runtime_error("GPU: más pedidos que la capacidad reservada");
    }
    const unsigned n = static_cast<unsigned>(records.size());
    const auto begin = std::chrono::steady_clock::now();
    ensure(cudaMemcpy(_d->records, records.data(), n * sizeof(OrderRecord), cudaMemcpyHostToDevice), "copia de ida");
    ensure(cudaMemset(_d->totals, 0, 4 * sizeof(unsigned long long)), "cudaMemset");
    void* start = nullptr;
    void* stop = nullptr;
#ifdef __CUDACC__
    start = _d->start;
    stop = _d->stop;
#endif
    return measured(timing, begin, _d->totals, start, stop, [&] {
        launch(phasesAoS, dim3(blocksFor(n)), dim3(kThreads), static_cast<const OrderRecord*>(_d->records), n,
               _d->totals);
    });
}

PhaseTotals PhasesGpu::fromColumns(const OrderColumns& columns, Timing* timing)
{
    if (columns.createdMs.size() > _capacity) {
        throw std::runtime_error("GPU: más pedidos que la capacidad reservada");
    }
    const unsigned n = static_cast<unsigned>(columns.createdMs.size());
    const std::vector<std::uint32_t>* host[4] = {&columns.createdMs, &columns.assignedMs, &columns.pickedUpMs,
                                                 &columns.deliveredMs};
    const auto begin = std::chrono::steady_clock::now();
    for (int k = 0; k < 4; ++k) {
        ensure(cudaMemcpy(_d->columns[k], host[k]->data(), n * sizeof(unsigned), cudaMemcpyHostToDevice),
               "copia de ida");
    }
    ensure(cudaMemset(_d->totals, 0, 4 * sizeof(unsigned long long)), "cudaMemset");
    void* start = nullptr;
    void* stop = nullptr;
#ifdef __CUDACC__
    start = _d->start;
    stop = _d->stop;
#endif
    return measured(timing, begin, _d->totals, start, stop, [&] {
        launch(phasesSoA, dim3(blocksFor(n)), dim3(kThreads), static_cast<const unsigned*>(_d->columns[0]),
               static_cast<const unsigned*>(_d->columns[1]), static_cast<const unsigned*>(_d->columns[2]),
               static_cast<const unsigned*>(_d->columns[3]), n, _d->totals);
    });
}
