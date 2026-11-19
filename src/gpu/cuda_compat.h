// cuda_compat.h: copiado sin cambios del repositorio del curso (lia-221-2026, rama concurrency-parallelism,
// ec2/common/cuda_compat.h, commit 947fb59). Solo se agregó este comentario.
//
// Permite que el mismo archivo .cu compile de dos maneras:
//  - con nvcc (USE_CUDA=ON, Google Colab): incluye el runtime de CUDA real;
//  - con g++ o clang como C++ (sin CUDA, la laptop): emula cada lanzamiento con un std::thread por hilo
//    del bloque y una barrera que espera con yield.
// La emulación sirve SOLO para comprobar que un kernel da el mismo resultado que la CPU (pruebas gpu_*_check).
// Nunca se mide (no dice nada del tiempo en una GPU) y nunca entra en delivery_sim: su barrera es espera
// activa y su estado global (blockDim, blockBarrier) admite un solo lanzamiento a la vez.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef __CUDACC__

#include <cuda_runtime.h>
#include <cuda/atomic>

#define CUDA_BACKEND "GPU (nvcc)"
#define BY_BACKEND(gpu, cpu) (gpu)

template <class Kernel, class... Args>
void launch(Kernel kernel, dim3 grid, dim3 block, Args... args) {
  kernel<<<grid, block>>>(args...);
}

#else

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#define CUDA_BACKEND "CPU emulation (threads)"
#define BY_BACKEND(gpu, cpu) (cpu)

#define __global__
#define __device__
#define __host__
#define __constant__
#define __shared__ static
#define __restrict__

struct dim3 {
  unsigned x;
  unsigned y;
  unsigned z;

  dim3(unsigned x_ = 1, unsigned y_ = 1, unsigned z_ = 1) : x(x_), y(y_), z(z_) {
  }
};

struct uint3 {
  unsigned x;
  unsigned y;
  unsigned z;
};

struct float4 {
  float x;
  float y;
  float z;
  float w;
};

inline thread_local uint3 threadIdx{0, 0, 0};
inline thread_local uint3 blockIdx{0, 0, 0};
inline dim3 blockDim;
inline dim3 gridDim;

constexpr int warpSize = 32;

namespace emu {

constexpr unsigned kWarpSize = warpSize;
constexpr unsigned kFullWarpMask = 0xffffffffu;
constexpr int kCudaAtomicFunctionOrder = __ATOMIC_RELAXED;

class Barrier {
 public:
  explicit Barrier(unsigned threadCount) : threadCount_(threadCount) {
  }

  void arriveAndWait() {
    const unsigned generationOnArrival = generation_.load(std::memory_order_acquire);
    const bool lastToArrive = arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == threadCount_;

    if (lastToArrive) {
      releaseEveryWaitingThread();
    } else {
      yieldUntilGenerationEnds(generationOnArrival);
    }
  }

 private:
  void releaseEveryWaitingThread() {
    arrived_.store(0, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  void yieldUntilGenerationEnds(unsigned generation) const {
    while (generation_.load(std::memory_order_acquire) == generation) {
      std::this_thread::yield();
    }
  }

  std::atomic<unsigned> arrived_{0};
  std::atomic<unsigned> generation_{0};
  const unsigned threadCount_;
};

inline Barrier* blockBarrier = nullptr;

using LaneBits = unsigned long long;

class Warp {
 public:
  explicit Warp(unsigned laneCount) : laneCount_(laneCount), barrier_(laneCount), published_{} {
  }

  unsigned laneCount() const {
    return laneCount_;
  }

  unsigned everyLaneMask() const {
    if (laneCount_ == kWarpSize) {
      return kFullWarpMask;
    }
    return (1u << laneCount_) - 1u;
  }

  bool hasLane(int lane) const {
    return lane >= 0 && lane < static_cast<int>(laneCount_);
  }

  void publish(unsigned lane, LaneBits bits) {
    __atomic_store_n(&published_[lane], bits, __ATOMIC_RELAXED);
  }

  LaneBits publishedBy(int lane) const {
    return __atomic_load_n(&published_[lane], __ATOMIC_RELAXED);
  }

  void waitForEveryLane() {
    barrier_.arriveAndWait();
  }

 private:
  const unsigned laneCount_;
  Barrier barrier_;
  LaneBits published_[kWarpSize];
};

inline thread_local Warp* warpOfThisThread = nullptr;
inline thread_local unsigned laneOfThisThread = 0;

template <class T>
LaneBits toLaneBits(T value) {
  static_assert(sizeof(T) <= sizeof(LaneBits), "shuffle of a type wider than 8 bytes");
  LaneBits bits = 0;
  std::memcpy(&bits, &value, sizeof(T));
  return bits;
}

template <class T>
T fromLaneBits(LaneBits bits) {
  T value;
  std::memcpy(&value, &bits, sizeof(T));
  return value;
}

template <class T>
T readValueOfLane(T myValue, int sourceLane) {
  Warp& myWarp = *warpOfThisThread;
  myWarp.publish(laneOfThisThread, toLaneBits(myValue));
  myWarp.waitForEveryLane();

  T received = myValue;
  if (myWarp.hasLane(sourceLane)) {
    received = fromLaneBits<T>(myWarp.publishedBy(sourceLane));
  }

  myWarp.waitForEveryLane();
  return received;
}

inline int positionInGroup(int lane, int width) {
  return lane % width;
}

inline int firstLaneOfGroup(int lane, int width) {
  return lane / width * width;
}

inline bool inSameGroup(int lane, int otherLane, int width) {
  return lane / width == otherLane / width;
}

inline int shflDownSourceLane(int lane, int delta, int width) {
  const bool sourceInGroup = positionInGroup(lane, width) + delta < width;
  return sourceInGroup ? lane + delta : lane;
}

inline int shflUpSourceLane(int lane, int delta, int width) {
  const bool sourceInGroup = positionInGroup(lane, width) >= delta;
  return sourceInGroup ? lane - delta : lane;
}

inline int shflXorSourceLane(int lane, int laneMask, int width) {
  const int partner = lane ^ laneMask;
  return inSameGroup(lane, partner, width) ? partner : lane;
}

inline int shflSourceLane(int lane, int srcLane, int width) {
  const int wrappedSrcLane = (srcLane % width + width) % width;
  return firstLaneOfGroup(lane, width) + wrappedSrcLane;
}

inline int myLane() {
  return static_cast<int>(laneOfThisThread);
}

}

inline void __syncthreads() {
  emu::blockBarrier->arriveAndWait();
}

inline void __syncwarp([[maybe_unused]] unsigned mask = emu::kFullWarpMask) {
  emu::warpOfThisThread->waitForEveryLane();
}

template <class T>
T __shfl_down_sync([[maybe_unused]] unsigned mask, T var, unsigned delta, int width = warpSize) {
  const int sourceLane = emu::shflDownSourceLane(emu::myLane(), static_cast<int>(delta), width);
  return emu::readValueOfLane(var, sourceLane);
}

template <class T>
T __shfl_up_sync([[maybe_unused]] unsigned mask, T var, unsigned delta, int width = warpSize) {
  const int sourceLane = emu::shflUpSourceLane(emu::myLane(), static_cast<int>(delta), width);
  return emu::readValueOfLane(var, sourceLane);
}

template <class T>
T __shfl_xor_sync([[maybe_unused]] unsigned mask, T var, int laneMask, int width = warpSize) {
  const int sourceLane = emu::shflXorSourceLane(emu::myLane(), laneMask, width);
  return emu::readValueOfLane(var, sourceLane);
}

template <class T>
T __shfl_sync([[maybe_unused]] unsigned mask, T var, int srcLane, int width = warpSize) {
  const int sourceLane = emu::shflSourceLane(emu::myLane(), srcLane, width);
  return emu::readValueOfLane(var, sourceLane);
}

inline unsigned __ballot_sync([[maybe_unused]] unsigned mask, int predicate) {
  unsigned ballot = 0;
  const unsigned laneCount = emu::warpOfThisThread->laneCount();

  for (unsigned sourceLane = 0; sourceLane < laneCount; ++sourceLane) {
    const int predicateOfLane =
        __shfl_sync(emu::kFullWarpMask, predicate, static_cast<int>(sourceLane));
    if (predicateOfLane) {
      ballot |= 1u << sourceLane;
    }
  }

  return ballot;
}

inline int __any_sync(unsigned mask, int predicate) {
  return __ballot_sync(mask, predicate) != 0;
}

inline int __all_sync(unsigned mask, int predicate) {
  return __ballot_sync(mask, predicate) == emu::warpOfThisThread->everyLaneMask();
}

namespace emu {

using WarpList = std::vector<std::unique_ptr<Warp>>;

inline unsigned threadCountOf(dim3 block) {
  return block.x * block.y * block.z;
}

inline WarpList splitIntoWarps(unsigned threadCount) {
  WarpList warps;

  for (unsigned firstThread = 0; firstThread < threadCount; firstThread += kWarpSize) {
    const unsigned threadsLeft = threadCount - firstThread;
    const unsigned laneCount = threadsLeft < kWarpSize ? threadsLeft : kWarpSize;
    warps.push_back(std::make_unique<Warp>(laneCount));
  }

  return warps;
}

inline uint3 threadIndexOf(unsigned linearThread, dim3 block) {
  const unsigned x = linearThread % block.x;
  const unsigned y = linearThread / block.x % block.y;
  const unsigned z = linearThread / (block.x * block.y);
  return {x, y, z};
}

inline void waitForTheWholeBlockToFinish() {
  blockBarrier->arriveAndWait();
}

template <class Kernel, class... Args>
void runThreadInEveryBlock(
    unsigned linearThread, Warp* threadWarp, dim3 grid, dim3 block, Kernel kernel, Args... args) {
  warpOfThisThread = threadWarp;
  laneOfThisThread = linearThread % kWarpSize;
  threadIdx = threadIndexOf(linearThread, block);

  for (unsigned bz = 0; bz < grid.z; ++bz) {
    for (unsigned by = 0; by < grid.y; ++by) {
      for (unsigned bx = 0; bx < grid.x; ++bx) {
        blockIdx = {bx, by, bz};
        kernel(args...);
        waitForTheWholeBlockToFinish();
      }
    }
  }
}

constexpr int kErrorInvalidConfiguration = 9;
constexpr unsigned kMaxThreadsPerBlock = 1024;
constexpr unsigned kMaxBlockDimZ = 64;
constexpr unsigned kMaxGridDimYZ = 65535;

inline int lastLaunchError = 0;

inline bool isValidLaunchConfiguration(dim3 grid, dim3 block) {
  const bool blockFits = block.x >= 1 && block.y >= 1 && block.z >= 1 &&
                         block.x <= kMaxThreadsPerBlock && block.y <= kMaxThreadsPerBlock &&
                         block.z <= kMaxBlockDimZ &&
                         threadCountOf(block) <= kMaxThreadsPerBlock;
  const bool gridFits = grid.x >= 1 && grid.y >= 1 && grid.z >= 1 && grid.y <= kMaxGridDimYZ &&
                        grid.z <= kMaxGridDimYZ;
  return blockFits && gridFits;
}

}

template <class Kernel, class... Args>
void launch(Kernel kernel, dim3 grid, dim3 block, Args... args) {
  if (!emu::isValidLaunchConfiguration(grid, block)) {
    emu::lastLaunchError = emu::kErrorInvalidConfiguration;
    return;
  }

  gridDim = grid;
  blockDim = block;

  const unsigned threadCount = emu::threadCountOf(block);
  emu::Barrier barrier(threadCount);
  emu::blockBarrier = &barrier;
  emu::WarpList warps = emu::splitIntoWarps(threadCount);

  std::vector<std::thread> threads;
  threads.reserve(threadCount);
  for (unsigned linearThread = 0; linearThread < threadCount; ++linearThread) {
    emu::Warp* threadWarp = warps[linearThread / emu::kWarpSize].get();
    threads.emplace_back([=] {
      emu::runThreadInEveryBlock(linearThread, threadWarp, grid, block, kernel, args...);
    });
  }

  for (std::thread& thread : threads) {
    thread.join();
  }
  emu::blockBarrier = nullptr;
}

namespace emu {

template <class T, class Improves>
T atomicReplaceWhileImproving(T* address, T candidate, Improves improves) {
  T current = __atomic_load_n(address, kCudaAtomicFunctionOrder);

  while (improves(candidate, current)) {
    const bool replaced = __atomic_compare_exchange_n(
        address, &current, candidate, true, kCudaAtomicFunctionOrder, kCudaAtomicFunctionOrder);
    if (replaced) {
      break;
    }
  }

  return current;
}

template <class T>
T atomicStoreMinimum(T* address, T value) {
  return atomicReplaceWhileImproving(address, value, [](T candidate, T current) {
    return candidate < current;
  });
}

template <class T>
T atomicStoreMaximum(T* address, T value) {
  return atomicReplaceWhileImproving(address, value, [](T candidate, T current) {
    return candidate > current;
  });
}

template <class T>
T atomicCompareAndSwap(T* address, T compare, T value) {
  T observed = compare;
  __atomic_compare_exchange_n(
      address, &observed, value, false, kCudaAtomicFunctionOrder, kCudaAtomicFunctionOrder);
  return observed;
}

}

inline int atomicAdd(int* address, int value) {
  return __atomic_fetch_add(address, value, emu::kCudaAtomicFunctionOrder);
}

inline unsigned atomicAdd(unsigned* address, unsigned value) {
  return __atomic_fetch_add(address, value, emu::kCudaAtomicFunctionOrder);
}

inline unsigned long long atomicAdd(unsigned long long* address, unsigned long long value) {
  return __atomic_fetch_add(address, value, emu::kCudaAtomicFunctionOrder);
}

inline float atomicAdd(float* address, float value) {
  float old;
  __atomic_load(address, &old, emu::kCudaAtomicFunctionOrder);
  float sum = old + value;

  while (!__atomic_compare_exchange(
      address, &old, &sum, true, emu::kCudaAtomicFunctionOrder, emu::kCudaAtomicFunctionOrder)) {
    sum = old + value;
  }

  return old;
}

inline int atomicMin(int* address, int value) {
  return emu::atomicStoreMinimum(address, value);
}

inline int atomicMax(int* address, int value) {
  return emu::atomicStoreMaximum(address, value);
}

inline unsigned long long atomicMin(unsigned long long* address, unsigned long long value) {
  return emu::atomicStoreMinimum(address, value);
}

inline unsigned atomicMin(unsigned* address, unsigned value) {
  return emu::atomicStoreMinimum(address, value);
}

inline unsigned atomicMax(unsigned* address, unsigned value) {
  return emu::atomicStoreMaximum(address, value);
}

inline int atomicSub(int* address, int value) {
  return __atomic_fetch_sub(address, value, emu::kCudaAtomicFunctionOrder);
}

inline int atomicExch(int* address, int value) {
  return __atomic_exchange_n(address, value, emu::kCudaAtomicFunctionOrder);
}

inline int atomicCAS(int* address, int compare, int value) {
  return emu::atomicCompareAndSwap(address, compare, value);
}

inline unsigned long long atomicCAS(unsigned long long* address,
                                    unsigned long long compare,
                                    unsigned long long value) {
  return emu::atomicCompareAndSwap(address, compare, value);
}

inline void __threadfence() {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

namespace cuda {

enum thread_scope {
  thread_scope_system = 0,
  thread_scope_device = 1,
  thread_scope_block = 2,
  thread_scope_thread = 3
};

namespace std {

enum memory_order {
  memory_order_relaxed = __ATOMIC_RELAXED,
  memory_order_consume = __ATOMIC_CONSUME,
  memory_order_acquire = __ATOMIC_ACQUIRE,
  memory_order_release = __ATOMIC_RELEASE,
  memory_order_acq_rel = __ATOMIC_ACQ_REL,
  memory_order_seq_cst = __ATOMIC_SEQ_CST
};

}

using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

template <class T, thread_scope Scope = thread_scope_system>
class atomic_ref {
 public:
  explicit atomic_ref(T& object) : address_(&object) {
  }

  T load(memory_order order = memory_order_seq_cst) const {
    T value;
    __atomic_load(address_, &value, order);
    return value;
  }

  void store(T value, memory_order order = memory_order_seq_cst) const {
    __atomic_store(address_, &value, order);
  }

  T exchange(T value, memory_order order = memory_order_seq_cst) const {
    T old;
    __atomic_exchange(address_, &value, &old, order);
    return old;
  }

  bool compare_exchange_strong(T& expected,
                               T desired,
                               memory_order order = memory_order_seq_cst) const {
    return __atomic_compare_exchange(address_, &expected, &desired, false, order, __ATOMIC_RELAXED);
  }

  T fetch_add(T value, memory_order order = memory_order_seq_cst) const {
    if constexpr (::std::is_integral_v<T>) {
      return __atomic_fetch_add(address_, value, order);
    } else {
      return fetchAddByCompareExchange(value, order);
    }
  }

  T fetch_sub(T value, memory_order order = memory_order_seq_cst) const {
    return fetch_add(-value, order);
  }

  T fetch_min(T value, memory_order order = memory_order_seq_cst) const {
    return replaceWhileImproving(value, order, [](T candidate, T current) {
      return candidate < current;
    });
  }

  T fetch_max(T value, memory_order order = memory_order_seq_cst) const {
    return replaceWhileImproving(value, order, [](T candidate, T current) {
      return candidate > current;
    });
  }

 private:
  T fetchAddByCompareExchange(T value, memory_order order) const {
    T old = load(memory_order_relaxed);
    bool replaced = false;
    while (!replaced) {
      replaced = compare_exchange_strong(old, old + value, order);
    }
    return old;
  }

  template <class Improves>
  T replaceWhileImproving(T candidate, memory_order order, Improves improves) const {
    T current = load(memory_order_relaxed);
    while (improves(candidate, current)) {
      if (compare_exchange_strong(current, candidate, order)) {
        break;
      }
    }
    return current;
  }

  T* address_;
};

}

enum cudaMemcpyKind {
  cudaMemcpyHostToDevice,
  cudaMemcpyDeviceToHost,
  cudaMemcpyDeviceToDevice,
  cudaMemcpyHostToHost
};

typedef int cudaError_t;

const cudaError_t cudaSuccess = 0;

const cudaError_t cudaErrorInvalidConfiguration = emu::kErrorInvalidConfiguration;

inline const char* cudaGetErrorString(cudaError_t error) {
  if (error == cudaErrorInvalidConfiguration) {
    return "invalid configuration argument";
  }
  return "no error";
}

inline cudaError_t cudaPeekAtLastError() {
  return emu::lastLaunchError;
}

inline cudaError_t cudaGetLastError() {
  const cudaError_t error = emu::lastLaunchError;
  emu::lastLaunchError = cudaSuccess;
  return error;
}

inline cudaError_t cudaDeviceSynchronize() {
  return cudaSuccess;
}

template <class T>
cudaError_t cudaMalloc(T** devicePointer, size_t bytes) {
  *devicePointer = static_cast<T*>(std::malloc(bytes));
  return cudaSuccess;
}

inline cudaError_t cudaFree(void* devicePointer) {
  std::free(devicePointer);
  return cudaSuccess;
}

inline cudaError_t cudaMemcpy(void* destination, const void* source, size_t bytes, cudaMemcpyKind) {
  std::memcpy(destination, source, bytes);
  return cudaSuccess;
}

inline cudaError_t cudaMemset(void* destination, int byteValue, size_t bytes) {
  std::memset(destination, byteValue, bytes);
  return cudaSuccess;
}

template <class T>
cudaError_t cudaMemcpyToSymbol(T& symbol, const void* source, size_t bytes) {
  std::memcpy(&symbol, source, bytes);
  return cudaSuccess;
}

struct cudaDeviceProp {
  char name[256];
  int major;
  int minor;
  int multiProcessorCount;
  int maxThreadsPerBlock;
  int maxThreadsDim[3];
  int maxGridSize[3];
  int maxThreadsPerMultiProcessor;
  int maxBlocksPerMultiProcessor;
  int warpSize;
  int regsPerBlock;
  int regsPerMultiprocessor;
  size_t sharedMemPerBlock;
  size_t sharedMemPerMultiprocessor;
  size_t totalGlobalMem;
  size_t totalConstMem;
  int clockRate;
};

inline cudaError_t cudaGetDeviceCount(int* deviceCount) {
  *deviceCount = 1;
  return cudaSuccess;
}

inline cudaError_t cudaGetDeviceProperties(cudaDeviceProp* properties, int) {
  constexpr int MAX_THREADS_PER_BLOCK = 1024;
  constexpr int MAX_GRID_X = 2147483647;
  constexpr int MAX_GRID_YZ = 65535;
  constexpr int MAX_BLOCK_Z = 64;
  constexpr size_t KIB = 1024;
  *properties = cudaDeviceProp{};
  std::snprintf(properties->name, sizeof properties->name, "CPU emulation (cuda_compat.h)");
  properties->multiProcessorCount = static_cast<int>(std::thread::hardware_concurrency());
  properties->maxThreadsPerBlock = MAX_THREADS_PER_BLOCK;
  properties->maxThreadsDim[0] = MAX_THREADS_PER_BLOCK;
  properties->maxThreadsDim[1] = MAX_THREADS_PER_BLOCK;
  properties->maxThreadsDim[2] = MAX_BLOCK_Z;
  properties->maxGridSize[0] = MAX_GRID_X;
  properties->maxGridSize[1] = MAX_GRID_YZ;
  properties->maxGridSize[2] = MAX_GRID_YZ;
  properties->maxThreadsPerMultiProcessor = 2 * MAX_THREADS_PER_BLOCK;
  properties->maxBlocksPerMultiProcessor = 32;
  properties->warpSize = warpSize;
  properties->regsPerBlock = 64 * 1024;
  properties->regsPerMultiprocessor = 64 * 1024;
  properties->sharedMemPerBlock = 48 * KIB;
  properties->sharedMemPerMultiprocessor = 100 * KIB;
  properties->totalConstMem = 64 * KIB;
  return cudaSuccess;
}

#endif

#define CHECK(call)                                                                          \
  do {                                                                                       \
    cudaError_t err_ = (call);                                                               \
    if (err_ != cudaSuccess) {                                                               \
      std::fprintf(                                                                          \
          stderr, "CUDA error %s at %s:%d\n", cudaGetErrorString(err_), __FILE__, __LINE__); \
      std::exit(1);                                                                          \
    }                                                                                        \
  } while (0)

struct Timer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  double ms() {
    cudaDeviceSynchronize();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    return std::chrono::duration<double, std::milli>(elapsed).count();
  }
};

inline unsigned cdiv(unsigned a, unsigned b) {
  return (a + b - 1) / b;
}

__host__ __device__ inline int imin(int a, int b) {
  return a < b ? a : b;
}

__host__ __device__ inline int imax(int a, int b) {
  return a > b ? a : b;
}

__host__ __device__ inline float fminf_(float a, float b) {
  return a < b ? a : b;
}

__host__ __device__ inline float fmaxf_(float a, float b) {
  return a > b ? a : b;
}
