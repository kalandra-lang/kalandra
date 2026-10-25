#include "../runtime/Tensor.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
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
using Vector = CpuTensor<Element, 1>;
using Matrix = CpuTensor<Element, 2>;
#if CASE == 4
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, Invocation *);
#elif CASE == 2
extern "C" Float _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Invocation *);
#else
extern "C" Float _mlir_ciface_kalandra_test_entry(Vector *, Vector *, Invocation *);
#endif
void check(bool valid, int line) {
  if (!valid)
    throw std::runtime_error("CPU optimization regression at line " + std::to_string(line));
}
#define CHECK(x) check(bool(x), __LINE__)
bool same(Float a, Float b) {
  return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b) || (std::isnan(a) && std::isnan(b));
}
// Independent recursive specification of the adjacent-pair tree, including odd carries.
Float tree(const Float *p, std::size_t n) {
  if (!n)
    return Float(0);
  if (n == 1)
    return *p;
  const auto split = std::bit_floor(n - 1);
  return tree(p, split) + tree(p + split, n - split);
}
// Fixed-seed bit patterns exercise mantissas and exponent gaps without a random-device dependency.
Float sample(std::size_t i, unsigned corpus) {
  if (corpus == 2) {
    std::uint64_t state = i + 0x9e3779b97f4a7c15ULL;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
    state ^= state >> 31;
    constexpr unsigned fraction = std::numeric_limits<Float>::digits - 1;
    constexpr unsigned bias = sizeof(Float) == 4 ? 127 : 1023;
    constexpr unsigned sign = sizeof(Float) * 8 - 1;
    const Bits bits = (Bits(state & 1) << sign) |
                      (Bits(bias - 30 + (state >> 1) % 61) << fraction) |
                      (Bits(state >> 8) & ((Bits(1) << fraction) - 1));
    return std::bit_cast<Float>(bits);
  }
  if (corpus == 3) {
    const Float epsilon = std::numeric_limits<Float>::epsilon();
    const Float rounding[] = {Float(1),
                              epsilon / 2,
                              Float(-1),
                              epsilon,
                              std::nextafter(Float(1), Float(2)),
                              std::nextafter(Float(1), Float(0)),
                              std::ldexp(Float(1), std::numeric_limits<Float>::digits),
                              -std::ldexp(Float(1), std::numeric_limits<Float>::digits),
                              Float(1) / Float(3),
                              Float(-1) / Float(7)};
    return rounding[i % std::size(rounding)];
  }
  if (corpus == 0)
    return Float(int((i * 11) % 31) - 15) / Float(8);
  const Float exceptional[] = {Float(0),
                               -Float(0),
                               Float(1),
                               Float(-1),
                               std::numeric_limits<Float>::denorm_min(),
                               -std::numeric_limits<Float>::denorm_min(),
                               std::numeric_limits<Float>::min(),
                               std::numeric_limits<Float>::max(),
                               std::numeric_limits<Float>::infinity(),
                               -std::numeric_limits<Float>::infinity(),
                               std::numeric_limits<Float>::quiet_NaN(),
                               Float(1.0000001),
                               Float(-0.0000001)};
  return exceptional[i % std::size(exceptional)];
}
#if CASE != 2 && CASE != 4
Float mapped(Float a, Float b) {
#if CASE == 0
  return a * b;
#elif CASE == 1
  const Float product = a * Float(3);
  return product + b;
#else
  const Float numerator = a + Float(1);
  const Float denominator = b + Float(2);
  return numerator / denominator;
#endif
}
void vectors(std::size_t n, std::int64_t step, unsigned corpus, std::size_t phase = 0) {
  const auto stride = std::abs(step);
  std::vector<Float> a(n * stride + 9), b(n * stride + 9), values(n);
  const auto offset = step < 0 && n ? 4 + (n - 1) * stride : 4;
  for (std::size_t i = 0; i < n; ++i) {
    a[offset + std::int64_t(i) * step] = sample(i + phase, corpus);
    b[offset + std::int64_t(i) * step] = sample(i * 3 + 5 + phase, corpus);
    values[i] = mapped(a[offset + std::int64_t(i) * step], b[offset + std::int64_t(i) * step]);
  }
  const auto oldA = a, oldB = b;
  Vector left{a.data(), a.data(), std::int64_t(offset), {std::int64_t(n)}, {step}};
  Vector right{b.data(), b.data(), std::int64_t(offset), {std::int64_t(n)}, {step}};
  Invocation context;
  const auto actual = _mlir_ciface_kalandra_test_entry(&left, &right, &context);
  CHECK(!context.status() && same(actual, tree(values.data(), n)));
  CHECK(n ? context.allocations() == 1 : context.allocations() <= 1);
  CHECK(std::memcmp(a.data(), oldA.data(), a.size() * sizeof(Float)) == 0);
  CHECK(std::memcmp(b.data(), oldB.data(), b.size() * sizeof(Float)) == 0);
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
  if (n == 513 && step == 1 && corpus == 0) {
    Invocation allocation;
    allocation.failAllocationAt(1);
    _mlir_ciface_kalandra_test_entry(&left, &right, &allocation);
    CHECK(allocation.status() == 2);
    allocation.settle();
    CHECK(!allocation.live());
    right.sizes[0] += 1;
    Invocation shape;
    _mlir_ciface_kalandra_test_entry(&left, &right, &shape);
    CHECK(shape.status() == 1);
    shape.settle();
    CHECK(!shape.live());
  }
}
void singleton(Float a, Float b) {
  Vector left{&a, &a, 0, {1}, {1}}, right{&b, &b, 0, {1}, {1}};
  Invocation context;
  const auto actual = _mlir_ciface_kalandra_test_entry(&left, &right, &context);
  CHECK(!context.status() && same(actual, mapped(a, b)));
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}
void cancellation() {
  constexpr std::size_t n = 1 << 22;
  std::vector<Float> data(n, Float(1));
  Vector input{data.data(), data.data(), 0, {n}, {1}};
  std::atomic<bool> cancelled{false}, finished{false}, ready{false};
  std::atomic<std::uint64_t> polls{0};
  Invocation context;
  context.bindCancellation(&cancelled);
  context.watchPolls(&polls);
  std::thread stop([&] {
    ready.store(true);
    while (polls.load() < 8 && !finished.load())
      std::atomic_signal_fence(std::memory_order_seq_cst);
    if (!finished.load())
      cancelled.store(true);
  });
  while (!ready.load())
    std::this_thread::yield();
  _mlir_ciface_kalandra_test_entry(&input, &input, &context);
  finished.store(true);
  stop.join();
  CHECK(cancelled.load() && context.poll() < 0 && !context.status());
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}
#else
void matrix(std::size_t m, std::size_t k, std::size_t n, std::size_t step, unsigned corpus,
            std::size_t phase = 0, bool reverseB = false) {
  const auto rowA = k * step + 3, rowB = n * step + 5;
  std::vector<Float> a(m * rowA + 9), b(k * rowB + 9), products(k), cells(m * n);
  const auto strideB = reverseB ? -std::ptrdiff_t(rowB) : std::ptrdiff_t(rowB);
  const auto offsetB = std::ptrdiff_t(4 + (reverseB && k ? (k - 1) * rowB : 0));
  for (std::size_t row = 0; row < m; ++row)
    for (std::size_t inner = 0; inner < k; ++inner)
      a[4 + row * rowA + inner * step] = sample(row * k + inner + phase, corpus);
  for (std::size_t row = 0; row < k; ++row)
    for (std::size_t col = 0; col < n; ++col)
      b[offsetB + std::ptrdiff_t(row) * strideB + col * step] =
          sample(row * n + col + 3 + phase, corpus);
  const auto oldA = a, oldB = b;
  for (std::size_t row = 0; row < m; ++row)
    for (std::size_t col = 0; col < n; ++col) {
      for (std::size_t inner = 0; inner < k; ++inner)
        products[inner] = a[4 + row * rowA + inner * step] *
                          b[offsetB + std::ptrdiff_t(inner) * strideB + col * step];
      cells[row * n + col] = tree(products.data(), k);
    }
  Matrix left{a.data(),
              a.data(),
              4,
              {std::int64_t(m), std::int64_t(k)},
              {std::int64_t(rowA), std::int64_t(step)}};
  Matrix right{b.data(),
               b.data(),
               offsetB,
               {std::int64_t(k), std::int64_t(n)},
               {strideB, std::int64_t(step)}};
  Invocation context;
#if CASE == 4
  Matrix actual{};
  _mlir_ciface_kalandra_test_entry(&actual, &left, &right, &context);
  CHECK(!context.status());
  CHECK(actual.sizes[0] == std::int64_t(m) && actual.sizes[1] == std::int64_t(n));
  for (std::size_t row = 0; row < m; ++row)
    for (std::size_t col = 0; col < n; ++col) {
      const auto value =
          actual.aligned[actual.offset + row * actual.strides[0] + col * actual.strides[1]];
      if (!same(value, cells[row * n + col])) {
        std::fprintf(stderr,
                     "matrix %zux%zux%zu corpus %u seed %zu cell (%zu,%zu): "
                     "actual=%a expected=%a\n",
                     m, k, n, corpus, phase, row, col, double(value), double(cells[row * n + col]));
        CHECK(false);
      }
    }
#else
  const auto actual = _mlir_ciface_kalandra_test_entry(&left, &right, &context);
  CHECK(!context.status() && same(actual, tree(cells.data(), cells.size())));
#endif
  if (m == 33 && k == 128 && n == 35 && step == 1 && corpus == 0) {
    for (std::uint64_t point = 1; point <= context.allocations(); ++point) {
      Invocation failed;
      failed.failAllocationAt(point);
#if CASE == 4
      Matrix result{};
      _mlir_ciface_kalandra_test_entry(&result, &left, &right, &failed);
      CHECK(result.aligned == nullptr);
#else
      CHECK(_mlir_ciface_kalandra_test_entry(&left, &right, &failed) == Float(0));
#endif
      CHECK(failed.status() == 2 && failed.callDepth() == 0);
      failed.settle();
      CHECK(!failed.live() && failed.allocations() == failed.releases());
    }
  }
  CHECK(std::memcmp(a.data(), oldA.data(), a.size() * sizeof(Float)) == 0);
  CHECK(std::memcmp(b.data(), oldB.data(), b.size() * sizeof(Float)) == 0);
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}
void matrixCancellation() {
  constexpr std::int64_t n = 512;
  std::vector<Float> data(n * n, Float(1));
  Matrix input{data.data(), data.data(), 0, {n, n}, {n, 1}};
  std::atomic<bool> cancelled{false}, finished{false}, ready{false};
  std::atomic<std::uint64_t> polls{0};
  Invocation context;
  context.bindCancellation(&cancelled);
  context.watchPolls(&polls);
  std::thread stop([&] {
    ready = true;
    while (polls.load() < 32 && !finished.load())
      std::atomic_signal_fence(std::memory_order_seq_cst);
    if (!finished.load())
      cancelled = true;
  });
  while (!ready.load())
    std::this_thread::yield();
#if CASE == 4
  Matrix result{};
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, &context);
  CHECK(result.aligned == nullptr);
#else
  CHECK(_mlir_ciface_kalandra_test_entry(&input, &input, &context) == Float(0));
#endif
  finished = true;
  stop.join();
  CHECK(cancelled.load() && context.poll() == -1 && !context.status() && context.callDepth() == 0);
  context.settle();
  CHECK(!context.live() && context.allocations() == context.releases());
}
#endif
int main() {
#if CASE != 2 && CASE != 4
  for (auto n : {0, 1, 2, 3, 17, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4097})
    for (auto stride : {1, 2, -1, -3})
      for (unsigned corpus : {0, 1, 2, 3})
        vectors(n, stride, corpus);
  // Isolate exceptional values so a NaN elsewhere cannot hide a finite failure.
  for (std::size_t phase = 0; phase < 39; ++phase)
    vectors(1, 1, 1, phase);
  for (std::size_t seed = 0; seed < 32; ++seed)
    vectors(257, seed % 2 ? -3 : 1, 2, seed * 4099);
  for (std::size_t a = 0; a < 13; ++a)
    for (std::size_t b = 0; b < 13; ++b)
      singleton(sample(a, 1), sample(b, 1));
  // 3*nextafter(1,2)-3 differs if the compiler illegally contracts multiply/add.
  singleton(std::nextafter(Float(1), Float(2)), Float(-3));
  cancellation();
#else
  for (auto k :
       {0, 1, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 95, 96, 97, 255, 256, 257, 513})
    for (auto rows : {0, 1, 3, 4, 5})
      for (auto columns : {0, 1, 2, 3, 4, 15, 16, 17})
        for (auto stride : {1, 2})
          matrix(rows, k, columns, stride, 0);
  for (auto k : {127, 128, 129, 159, 160, 161, 255, 256, 257, 511, 512, 513})
    for (unsigned corpus : {0, 2, 3})
      matrix(33, k, 35, 1, corpus, 17);
  matrix(33, 129, 35, 1, 2, 17, true);
  matrix(33, 129, 35, 2, 2, 17);
  for (auto k : {1, 3, 17, 257})
    matrix(5, k, 17, 2, 1);
  for (auto k : {1, 3, 17, 255, 256, 257, 513})
    for (unsigned corpus : {2, 3})
      for (std::size_t seed = 0; seed < 8; ++seed)
        matrix(5, k, 17, seed % 2 + 1, corpus, seed * 4099);
  for (std::size_t phase = 0; phase < 39; ++phase)
    matrix(1, 1, 1, 1, 1, phase);
  matrixCancellation();
#endif
  std::puts("CPU optimized tree order, tails, strides, immutability, checked failures and cleanup "
            "passed");
}
