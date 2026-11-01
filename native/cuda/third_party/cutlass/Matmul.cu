// Kalandra descriptor adapters for the pinned BSD-3-Clause CUTLASS C++ headers.
// All operands and accumulators are float32 SIMT. No Tensor Core operations.
// Five independent K partitions; one deterministic eight-leaf reduction.
// No float atomics, inter-CTA waits or reads of previous output contents.
#include <cutlass/epilogue/thread/linear_combination.h>
#include <cutlass/gemm/kernel/default_gemm.h>
struct KalandraSwizzle : cutlass::gemm::threadblock::GemmIdentityThreadblockSwizzle<1> {
  CUTLASS_DEVICE cutlass::gemm::GemmCoord get_tile_offset(int) const {
    return {int(blockIdx.y), int(blockIdx.x), 0};
  }
};
using Epilogue = cutlass::epilogue::thread::LinearCombination<
    float, 1, float, float, cutlass::epilogue::thread::ScaleType::OnlyAlphaScaling>;
using Gemm = typename cutlass::gemm::kernel::DefaultGemm<
    float, cutlass::layout::RowMajor, 1, float, cutlass::layout::RowMajor, 1, float,
    cutlass::layout::RowMajor, float, cutlass::arch::OpClassSimt, cutlass::arch::Sm80,
    cutlass::gemm::GemmShape<64, 128, 8>, cutlass::gemm::GemmShape<64, 32, 8>,
    cutlass::gemm::GemmShape<1, 1, 1>, Epilogue, KalandraSwizzle, 5, false,
    cutlass::arch::OpMultiplyAdd>::GemmKernel;

using GemmLarge = typename cutlass::gemm::kernel::DefaultGemm<
    float, cutlass::layout::RowMajor, 1, float, cutlass::layout::RowMajor, 1, float,
    cutlass::layout::RowMajor, float, cutlass::arch::OpClassSimt, cutlass::arch::Sm80,
    cutlass::gemm::GemmShape<128, 256, 8>, cutlass::gemm::GemmShape<64, 64, 8>,
    cutlass::gemm::GemmShape<1, 1, 1>, Epilogue, KalandraSwizzle, 4, false,
    cutlass::arch::OpMultiplyAdd>::GemmKernel;
extern "C" __global__
__launch_bounds__(128) void kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1(
    unsigned long long b0a, unsigned long long b0p, unsigned long long b0o, unsigned long long b0n,
    unsigned long long b0s, unsigned long long b1a, unsigned long long b1p, unsigned long long b1o,
    unsigned long long b1n, unsigned long long b1s, unsigned long long b2a, unsigned long long b2p,
    unsigned long long b2o, unsigned long long b2n, unsigned long long b2s, unsigned long long b3a,
    unsigned long long b3p, unsigned long long b3o, unsigned long long b3n,
    unsigned long long b3s) {
  const unsigned *p = reinterpret_cast<const unsigned *>(b3p) + b3o;
  int M = p[0], N = p[2], K = p[1];
  unsigned long long count = (unsigned long long)M * N;
  float *A = reinterpret_cast<float *>(b0p) + b0o;
  float *B = reinterpret_cast<float *>(b1p) + b1o;
  float *C = reinterpret_cast<float *>(b2p) + b2o + count * blockIdx.z;
  int slice = ((K + 5 * 8 - 1) / (5 * 8)) * 8, begin = blockIdx.z * slice;
  if (begin >= K) {
    for (unsigned i = threadIdx.x; i < 128 * 64; i += blockDim.x) {
      unsigned row = blockIdx.y * 64 + i / 128, col = blockIdx.x * 128 + i % 128;
      if (row < unsigned(M) && col < unsigned(N))
        C[(unsigned long long)row * N + col] = 0.0f;
    }
    return;
  }
  int localK = K - begin < slice ? K - begin : slice;
  A += begin;
  B += (unsigned long long)begin * N;
  __shared__ Gemm::SharedStorage storage;
  Gemm::Params params({M, N, localK}, {(M + 63) / 64, (N + 127) / 128, 1},
                      {A, cutlass::layout::RowMajor(K)}, {B, cutlass::layout::RowMajor(N)},
                      {C, cutlass::layout::RowMajor(N)}, {C, cutlass::layout::RowMajor(N)},
                      Epilogue::Params(1.0f));
  Gemm op;
  op(params, storage);
}

extern "C" __global__
__launch_bounds__(256) void kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1_large(
    unsigned long long b0a, unsigned long long b0p, unsigned long long b0o, unsigned long long b0n,
    unsigned long long b0s, unsigned long long b1a, unsigned long long b1p, unsigned long long b1o,
    unsigned long long b1n, unsigned long long b1s, unsigned long long b2a, unsigned long long b2p,
    unsigned long long b2o, unsigned long long b2n, unsigned long long b2s, unsigned long long b3a,
    unsigned long long b3p, unsigned long long b3o, unsigned long long b3n,
    unsigned long long b3s) {
  const unsigned *p = reinterpret_cast<const unsigned *>(b3p) + b3o;
  int M = p[0], N = p[2], K = p[1];
  unsigned long long count = (unsigned long long)M * N;
  float *A = reinterpret_cast<float *>(b0p) + b0o;
  float *B = reinterpret_cast<float *>(b1p) + b1o;
  float *C = reinterpret_cast<float *>(b2p) + b2o + count * blockIdx.z;
  int slice = ((K + 5 * 8 - 1) / (5 * 8)) * 8, begin = blockIdx.z * slice;
  if (begin >= K) {
    for (unsigned i = threadIdx.x; i < 256 * 128; i += blockDim.x) {
      unsigned row = blockIdx.y * 128 + i / 256, col = blockIdx.x * 256 + i % 256;
      if (row < unsigned(M) && col < unsigned(N))
        C[(unsigned long long)row * N + col] = 0.0f;
    }
    return;
  }
  int localK = K - begin < slice ? K - begin : slice;
  A += begin;
  B += (unsigned long long)begin * N;
  __shared__ GemmLarge::SharedStorage storage;
  GemmLarge::Params params({M, N, localK}, {(M + 127) / 128, (N + 255) / 256, 1},
                           {A, cutlass::layout::RowMajor(K)}, {B, cutlass::layout::RowMajor(N)},
                           {C, cutlass::layout::RowMajor(N)}, {C, cutlass::layout::RowMajor(N)},
                           Epilogue::Params(1.0f));
  GemmLarge op;
  op(params, storage);
}

extern "C" __global__
__launch_bounds__(256) void kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1_reduce(
    unsigned long long b0a, unsigned long long b0p, unsigned long long b0o, unsigned long long b0n,
    unsigned long long b0s, unsigned long long b1a, unsigned long long b1p, unsigned long long b1o,
    unsigned long long b1n, unsigned long long b1s, unsigned long long b2a, unsigned long long b2p,
    unsigned long long b2o, unsigned long long b2n, unsigned long long b2s, unsigned long long b3a,
    unsigned long long b3p, unsigned long long b3o, unsigned long long b3n,
    unsigned long long b3s) {
  const unsigned *p = reinterpret_cast<const unsigned *>(b3p) + b3o;
  const unsigned long long count = (unsigned long long)p[0] * p[2],
                           i = (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= count)
    return;
  const float *input = reinterpret_cast<const float *>(b0p) + b0o;
  float *output = reinterpret_cast<float *>(b2p) + b2o;
  float v[8] = {0};
#pragma unroll
  for (unsigned j = 0; j < 5; ++j)
    v[j] = input[j * count + i];
#pragma unroll
  for (unsigned step = 1; step < 8; step *= 2) {
#pragma unroll
    for (unsigned j = 0; j < 8; j += step * 2)
      v[j] = v[j] + v[j + step];
  }
  output[i] = v[0];
}

extern "C" __global__
__launch_bounds__(256) void kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1_no_scratch(
    unsigned long long b0a, unsigned long long b0p, unsigned long long b0o, unsigned long long b0n,
    unsigned long long b0s, unsigned long long b1a, unsigned long long b1p, unsigned long long b1o,
    unsigned long long b1n, unsigned long long b1s, unsigned long long b2a, unsigned long long b2p,
    unsigned long long b2o, unsigned long long b2n, unsigned long long b2s, unsigned long long b3a,
    unsigned long long b3p, unsigned long long b3o, unsigned long long b3n,
    unsigned long long b3s) {
  const unsigned *p = reinterpret_cast<const unsigned *>(b3p) + b3o;
  const unsigned long long m = p[0], k = p[1], n = p[2],
                           index = (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= m * n)
    return;
  const unsigned long long row = index / n, col = index % n,
                           slice = ((k + 5 * 8 - 1) / (5 * 8)) * 8;
  const float *A = reinterpret_cast<const float *>(b0p) + b0o;
  const float *B = reinterpret_cast<const float *>(b1p) + b1o;
  float *C = reinterpret_cast<float *>(b2p) + b2o;
  float v[8] = {0};
#pragma unroll
  for (unsigned j = 0; j < 5; ++j) {
    const unsigned long long begin = j * slice;
    if (begin < k) {
      const unsigned long long length = k - begin < slice ? k - begin : slice;
      float acc = 0;
      for (unsigned long long t = 0; t < length; ++t)
        acc = __fmaf_rn(A[row * k + begin + t], B[(begin + t) * n + col], acc);
      for (unsigned long long t = length; t % 8; ++t)
        acc = __fmaf_rn(0.0f, 0.0f, acc);
      v[j] = acc;
    }
  }
#pragma unroll
  for (unsigned step = 1; step < 8; step *= 2) {
#pragma unroll
    for (unsigned j = 0; j < 8; j += step * 2)
      v[j] += v[j + step];
  }
  C[index] = v[0];
}
