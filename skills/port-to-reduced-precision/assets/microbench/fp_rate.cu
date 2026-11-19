// SPDX-License-Identifier: Apache-2.0

/// Rough FP32 : FP64 throughput ratio of the current CUDA GPU.
///
/// The ratio R decides which remedies of a reduced-precision port pay off on
/// a given device: on parts with R near 2 a `double` accumulator is nearly
/// free and emulated arithmetic loses, on parts with R of 32 or more every
/// FP64 instruction and every float<->double conversion is expensive.
///
/// Each thread runs eight independent fused multiply-add chains, so the
/// kernel is bound by arithmetic throughput and touches no memory. The same
/// kernel is timed in float and in double.
///
/// Build and run:
///     nvcc -O2 -arch=native -o fp_rate fp_rate.cu && ./fp_rate
/// (pass the device's architecture, e.g. -arch=sm_90, if your nvcc does not
/// accept -arch=native.)
///
/// This is a classification aid, not a benchmark: clocks, power caps and
/// thermal state move the absolute rates. Read the ratio to one significant
/// digit and prefer the vendor's figures when they exist.

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

template <class T>
__global__ void fma_chains(T* out, int iterations, T a, T b) {
  const T seed = T(1) + T(threadIdx.x) * T(1.0e-3);
  T x0 = seed, x1 = seed + T(0.125), x2 = seed + T(0.25), x3 = seed + T(0.375);
  T x4 = seed + T(0.5), x5 = seed + T(0.625), x6 = seed + T(0.75), x7 = seed + T(0.875);
  for (int i = 0; i < iterations; ++i) {
    x0 = x0 * a + b;
    x1 = x1 * a + b;
    x2 = x2 * a + b;
    x3 = x3 * a + b;
    x4 = x4 * a + b;
    x5 = x5 * a + b;
    x6 = x6 * a + b;
    x7 = x7 * a + b;
  }
  out[blockIdx.x * blockDim.x + threadIdx.x] = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
}

#define CHECK(call)                                                           \
  do {                                                                        \
    const cudaError_t err__ = (call);                                         \
    if (err__ != cudaSuccess) {                                               \
      std::fprintf(stderr, "CUDA error %s at %s:%d: %s\n", #call, __FILE__,   \
                   __LINE__, cudaGetErrorString(err__));                      \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

/// FMAs per second of one precision; the best of several timed launches.
template <class T>
double fma_rate(int blocks, int threads, int iterations) {
  T* out = nullptr;
  CHECK(cudaMalloc(&out, sizeof(T) * static_cast<size_t>(blocks) * threads));
  cudaEvent_t start, stop;
  CHECK(cudaEventCreate(&start));
  CHECK(cudaEventCreate(&stop));
  // a contracts towards the fixed point b / (1 - a) = 1: values stay finite
  const T a = T(0.999), b = T(0.001);
  fma_chains<T><<<blocks, threads>>>(out, iterations, a, b);  // warm-up
  CHECK(cudaGetLastError());
  CHECK(cudaDeviceSynchronize());
  double best = 0.0;
  for (int repeat = 0; repeat < 5; ++repeat) {
    CHECK(cudaEventRecord(start));
    fma_chains<T><<<blocks, threads>>>(out, iterations, a, b);
    CHECK(cudaGetLastError());
    CHECK(cudaEventRecord(stop));
    CHECK(cudaEventSynchronize(stop));
    float ms = 0.0f;
    CHECK(cudaEventElapsedTime(&ms, start, stop));
    const double fmas = 8.0 * iterations * static_cast<double>(blocks) * threads;
    const double rate = fmas / (1.0e-3 * static_cast<double>(ms));
    if (rate > best) best = rate;
  }
  CHECK(cudaEventDestroy(start));
  CHECK(cudaEventDestroy(stop));
  CHECK(cudaFree(out));
  return best;
}

int main() {
  int device = 0;
  CHECK(cudaGetDevice(&device));
  cudaDeviceProp prop;
  CHECK(cudaGetDeviceProperties(&prop, device));
  const int threads = 256;
  const int blocks = 32 * prop.multiProcessorCount;  // several blocks per SM
  const double rate32 = fma_rate<float>(blocks, threads, 1 << 14);
  const double rate64 = fma_rate<double>(blocks, threads, 1 << 11);
  const double ratio = rate32 / rate64;

  std::printf("device: %s (compute capability %d.%d, %d SMs)\n", prop.name,
              prop.major, prop.minor, prop.multiProcessorCount);
  std::printf("FP32 FMA rate: %.3g /s\n", rate32);
  std::printf("FP64 FMA rate: %.3g /s\n", rate64);
  std::printf("FP32:FP64 throughput ratio R: %.1f\n", ratio);
  const char* fp64_class = ratio < 6.0    ? "native (FP64 runs near half the FP32 rate)"
                           : ratio < 16.0 ? "intermediate"
                                          : "rationed (FP64 is scarce on this device)";
  std::printf("fp64 class: %s\n", fp64_class);
  return 0;
}
