#include "../runtime/Tensor.h"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
#ifdef FLOAT32
using Float = float;
using Bits = std::uint32_t;
constexpr auto Element = NumericType::Float32;
#else
using Float = double;
using Bits = std::uint64_t;
constexpr auto Element = NumericType::Float64;
#endif
using Matrix = CpuTensor<Element, 2>;
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, Invocation *);
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const auto n = std::stoll(argv[1]);
  if (n < 1 || n > 1024)
    return 2;
  std::vector<Float> a(n * n), b(n * n);
  for (std::size_t i = 0; i < a.size(); ++i) {
    a[i] = Float(int(i % 17) - 8) / Float(8);
    b[i] = Float(int(i % 13) - 6) / Float(8);
  }
  // Independently compute all possible row/column phases in integer arithmetic.
  // This checks every cell without using the compiler kernel's tiles or tree.
  std::int64_t exact[17][13]{};
  for (std::int64_t rowPhase = 0; rowPhase < 17; ++rowPhase)
    for (std::int64_t colPhase = 0; colPhase < 13; ++colPhase)
      for (std::int64_t k = 0; k < n; ++k)
        exact[rowPhase][colPhase] += ((rowPhase + k) % 17 - 8) * ((colPhase + k * n) % 13 - 6);
  Matrix left{a.data(), a.data(), 0, {n, n}, {n, 1}};
  Matrix right{b.data(), b.data(), 0, {n, n}, {n, 1}};
  Matrix result{};
  Invocation context;
  _mlir_ciface_kalandra_test_entry(&result, &left, &right, &context);
  if (context.status() || context.callDepth() || result.sizes[0] != n || result.sizes[1] != n)
    throw std::runtime_error("benchmark matrix admission/shape failure");
  for (std::int64_t row = 0; row < n; ++row)
    for (std::int64_t col = 0; col < n; ++col) {
      const auto actual =
          result.aligned[result.offset + row * result.strides[0] + col * result.strides[1]];
      const auto expected = Float(exact[(row * n) % 17][col % 13]) / Float(64);
      if (std::bit_cast<Bits>(actual) != std::bit_cast<Bits>(expected))
        throw std::runtime_error("benchmark matrix cell mismatch at " + std::to_string(row) + "," +
                                 std::to_string(col));
    }
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != Float(int(i % 17) - 8) / Float(8) || b[i] != Float(int(i % 13) - 6) / Float(8))
      throw std::runtime_error("benchmark matrix mutated an input");
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    throw std::runtime_error("benchmark matrix leaked");
  std::printf("%lldx%lld: %lld exact benchmark-input cells passed\n", (long long)n, (long long)n,
              (long long)(n * n));
}
