#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#ifdef BENCH_FLOAT32
using Float = float;
#else
using Float = double;
#endif
#ifdef KALANDRA_KERNEL
#include "../../runtime/Tensor.h"
using namespace kalandra;
using namespace kalandra::runtime;
constexpr NumericType Element = sizeof(Float) == 4 ? NumericType::Float32 : NumericType::Float64;
using Vector = CpuTensor<Element, 1>;
using Matrix = CpuTensor<Element, 2>;
#ifdef MATRIX_KERNEL
extern "C" Float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Invocation *);
#else
extern "C" Float _mlir_ciface_kalandra_test_entry(Vector *, Vector *, Invocation *);
#endif
#else
extern "C" Float benchmark_kernel(int, std::size_t, const Float *, const Float *);
#endif

int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  const int kind = std::atoi(argv[1]);
  const std::size_t n = std::stoull(argv[2]), iterations = std::stoull(argv[3]);
  const int rounds = std::atoi(argv[4]);
  if (kind < 0 || kind > 2 || !n || !iterations || rounds < 1)
    return 2;
  const auto count = kind == 2 ? n * n : n;
  std::vector<Float> a(count), b(count);
  for (std::size_t i = 0; i < count; ++i) {
    a[i] = (int(i % 17) - 8) / 8.0;
    b[i] = (int(i % 13) - 6) / 8.0;
  }
  for (int round = -2; round < rounds; ++round) {
    double checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
#ifdef KALANDRA_KERNEL
      Invocation context;
#ifdef MATRIX_KERNEL
      Matrix left{a.data(), a.data(), 0, {std::int64_t(n), std::int64_t(n)}, {std::int64_t(n), 1}};
      Matrix right{b.data(), b.data(), 0, {std::int64_t(n), std::int64_t(n)}, {std::int64_t(n), 1}};
#else
      Vector left{a.data(), a.data(), 0, {std::int64_t(n)}, {1}};
      Vector right{b.data(), b.data(), 0, {std::int64_t(n)}, {1}};
#endif
      checksum += _mlir_ciface_kalandra_test_entry(&left, &right, &context);
      if (context.status())
        throw std::runtime_error("Kalandra kernel failed");
      context.settle();
      if (context.live())
        throw std::runtime_error("Kalandra kernel leaked storage");
#else
      checksum += benchmark_kernel(kind, n, a.data(), b.data());
#endif
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    // Warmup results are emitted too, so the orchestrator validates every batch.
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g}\n", round, ns, checksum);
  }
  for (std::size_t i = 0; i < count; ++i)
    if (a[i] != (int(i % 17) - 8) / 8.0 || b[i] != (int(i % 13) - 6) / 8.0)
      return 3;
}
