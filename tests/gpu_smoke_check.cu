// Prueba de humo de la compilación con CUDA. El mismo archivo corre en una GPU real (nvcc, USE_CUDA=ON, Colab)
// o emulado con hilos en la CPU (g++ o clang como C++, con src/gpu/cuda_compat.h). Suma dos vectores de
// enteros en la GPU y compara con un bucle en la CPU: con enteros la igualdad es exacta. El largo no es
// múltiplo del bloque, así que el último bloque tiene hilos que no deben escribir (el `if (i < n)`).
// No lanza nada inválido a propósito: así compute-sanitizer debe dar 0 errores. Imprime el backend y el dispositivo usados. Sale con 0 si todo coincide.
#include <cstdio>
#include <vector>

#include "gpu/cuda_compat.h"

namespace {

__global__ void addVectors(const int* a, const int* b, int* sum, int n)
{
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n) {
        sum[i] = a[i] + b[i];
    }
}

bool check(const char* what, bool ok)
{
    std::printf("%s: %s\n", what, ok ? "OK" : "FALLA");
    return ok;
}

}  // namespace

int main()
{
    cudaDeviceProp device;
    CHECK(cudaGetDeviceProperties(&device, 0));
    std::printf("backend: %s · dispositivo: %s (sm_%d%d, %d SM)\n", CUDA_BACKEND, device.name, device.major,
                device.minor, device.multiProcessorCount);

    const int n = 20000 + 37;
    const unsigned threads = 256;
    std::vector<int> a(n), b(n), fromGpu(n, -1), fromCpu(n);
    for (int i = 0; i < n; ++i) {
        a[i] = i * 7 - 3000;
        b[i] = 50000 - i * 3;
        fromCpu[i] = a[i] + b[i];
    }

    const size_t bytes = n * sizeof(int);
    int *dA = nullptr, *dB = nullptr, *dSum = nullptr;
    CHECK(cudaMalloc(&dA, bytes));
    CHECK(cudaMalloc(&dB, bytes));
    CHECK(cudaMalloc(&dSum, bytes));
    CHECK(cudaMemcpy(dA, a.data(), bytes, cudaMemcpyHostToDevice));
    CHECK(cudaMemcpy(dB, b.data(), bytes, cudaMemcpyHostToDevice));
    CHECK(cudaMemset(dSum, 0xff, bytes));  // -1 en todos: un elemento sin escribir no pasa por correcto

    launch(addVectors, dim3(cdiv(n, threads)), dim3(threads), static_cast<const int*>(dA),
           static_cast<const int*>(dB), dSum, n);
    CHECK(cudaGetLastError());
    CHECK(cudaDeviceSynchronize());
    CHECK(cudaMemcpy(fromGpu.data(), dSum, bytes, cudaMemcpyDeviceToHost));

    const bool ok = check("suma de vectores: GPU = CPU, elemento por elemento", fromGpu == fromCpu);

    CHECK(cudaFree(dA));
    CHECK(cudaFree(dB));
    CHECK(cudaFree(dSum));
    return ok ? 0 : 1;
}
