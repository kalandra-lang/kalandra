#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using kalandra::runtime::Invocation;
struct Matrix {
  float *allocated, *aligned;
  std::int64_t offset, sizes[2], strides[2];
};
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, Invocation *);
static void check(bool ok) {
  if (!ok) {
    std::fputs("Matrix cell/ownership conformance failed\n", stderr);
    std::exit(1);
  }
}
static float balanced(std::vector<float> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    std::vector<float> next;
    for (std::size_t i = 0; i < values.size(); i += 2) {
      if (i + 1 == values.size())
        next.push_back(values[i]);
      else {
        volatile float rounded = values[i] + values[i + 1];
        next.push_back(static_cast<float>(rounded));
      }
    }
    values = std::move(next);
  }
  return values.front();
}
static unsigned verify(std::int64_t rows, std::int64_t inner, std::int64_t columns, unsigned mode,
                       bool alias = false, unsigned fail = 0) {
  // Inputs are float-aligned but need not be 16-byte aligned. Sentinel elements
  // make prefix/suffix corruption visible as well as changes to actual inputs.
  std::vector<float> a(rows * inner + 2, 123), b(inner * columns + 2, 456);
  std::uint32_t seed = 0x12345678;
  auto value = [&](unsigned i) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    static constexpr std::uint32_t edges[] = {
        0,          0x80000000, 1,          0x80000001, 0x007fffff, 0x00800000, 0x7f7fffff,
        0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001, 0x3f800001, 0xbf800000};
    if (mode == 0)
      return float(int(i % 17) - 8) / 8;
    if (mode == 1)
      return std::bit_cast<float>((seed & 0x807fffff) | (((seed >> 23) % 46 + 100) << 23));
    return std::bit_cast<float>(edges[i % std::size(edges)]);
  };
  for (std::size_t i = 1; i + 1 < a.size(); ++i)
    a[i] = value(i);
  for (std::size_t i = 1; i + 1 < b.size(); ++i)
    b[i] = value(i + 7);
  auto originalA = a, originalB = b;
  Matrix left{a.data(), a.data() + 1, 0, {rows, inner}, {inner, 1}};
  Matrix right{b.data(), b.data() + 1, 0, {inner, columns}, {columns, 1}};
  if (alias) {
    check(rows == inner && inner == columns);
  }
  const auto &referenceRight = alias ? left : right;
  unsigned allocations = 0;
  for (unsigned repeat = 0; repeat < (fail ? 1U : 2U); ++repeat) {
    Invocation context;
    context.failAllocationAt(fail);
    Matrix result{};
    _mlir_ciface_kalandra_test_entry(&result, &left, &right, &context);
    if (fail)
      check(context.status() == 2);
    else {
      check(!context.status() && result.sizes[0] == rows && result.sizes[1] == columns);
      for (std::int64_t row = 0; row < rows; ++row)
        for (std::int64_t col = 0; col < columns; ++col) {
          std::vector<float> products;
          for (std::int64_t k = 0; k < inner; ++k) {
            volatile float rounded =
                left.aligned[row * inner + k] * referenceRight.aligned[k * columns + col];
            products.push_back(static_cast<float>(rounded));
          }
          auto expected = balanced(std::move(products));
          auto actual =
              result.aligned[result.offset + row * result.strides[0] + col * result.strides[1]];
          if (!(std::isnan(actual) && std::isnan(expected)) &&
              std::bit_cast<std::uint32_t>(actual) != std::bit_cast<std::uint32_t>(expected)) {
            std::fprintf(stderr, "Mismatch: %lld x %lld x %lld cell %lld,%lld mode %u\n",
                         (long long)rows, (long long)inner, (long long)columns, (long long)row,
                         (long long)col, mode);
            check(false);
          }
        }
    }
    check(!std::memcmp(a.data(), originalA.data(), a.size() * sizeof(float)) &&
          !std::memcmp(b.data(), originalB.data(), b.size() * sizeof(float)));
    allocations = context.allocations();
    // Dense wide shapes also pack the RHS once; allocation count remains
    // independent of tile count and every reserved backing is swept below.
    const bool packed = rows >= 32 && inner >= 128 && columns >= 32;
    check(allocations <= (packed ? 3u : 2u));
    context.settle();
    check(!context.live() && context.allocations() == context.releases());
  }
  return allocations;
}
int main() {
  unsigned cases = 0;
#ifndef KALANDRA_MATRIX_SHARED
  for (auto rows : {0, 1, 3})
    for (auto inner : {0, 1, 2, 3, 7, 16, 33})
      for (auto columns : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 17})
        for (unsigned mode = 0; mode < 3; ++mode) {
          verify(rows, inner, columns, mode);
          ++cases;
        }
  for (auto rows : {4, 5, 6, 7, 8, 9})
    for (auto inner : {0, 1, 3, 255, 256, 257, 511, 512, 513, 767, 768, 769, 1023, 1024, 1025})
      for (auto columns : {16, 17, 18, 19, 20, 21, 22, 23, 31, 32, 33})
        for (unsigned mode = 0; mode < 3; ++mode) {
          verify(rows, inner, columns, mode);
          ++cases;
        }
  // Many block roots plus every small-lane column edge, with all row edges.
  for (auto rows : {4, 7})
    for (auto columns : {17, 31, 33})
      for (unsigned mode = 0; mode < 3; ++mode) {
        verify(rows, 8193, columns, mode);
        ++cases;
      }
  // Exercise every allocation failure for scalar-only, pair-only, full tiles,
  // both tail forms, and empty contractions/outputs.
  for (auto inner : {0, 7})
    for (auto columns : {0, 1, 2, 3, 4, 6, 7}) {
      auto allocations = verify(3, inner, columns, 1);
      for (unsigned position = 1; position <= allocations; ++position)
        verify(3, inner, columns, 1, false, position);
    }
  for (const auto &shape : {std::array{4, 0, 16}, std::array{4, 256, 16}, std::array{4, 257, 16},
                            std::array{5, 257, 17}, std::array{6, 513, 18}, std::array{7, 8193, 31},
                            std::array{8, 257, 32}, std::array{7, 257, 33}}) {
    auto [rows, inner, columns] = shape;
    auto allocations = verify(rows, inner, columns, 1);
    for (unsigned position = 1; position <= allocations; ++position)
      verify(rows, inner, columns, 1, false, position);
  }
#else
  // The source shared() helper uses a twice; its two wrapper inputs are still
  // independently owned, respecting bufferization's restricted-input contract.
  for (auto n : {2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 31, 32, 33})
    for (unsigned mode = 0; mode < 3; ++mode) {
      verify(n, n, n, mode, true);
      ++cases;
    }
  verify(257, 257, 257, 1, true);
  ++cases;
  for (auto n : {2, 3, 5, 6, 7, 16, 17, 257}) {
    auto allocations = verify(n, n, n, 1, true);
    for (unsigned position = 1; position <= allocations; ++position)
      verify(n, n, n, 1, true, position);
  }
#endif
  std::atomic<bool> cancelled{true};
  Invocation context;
  context.bindCancellation(&cancelled);
  float input = 1, other = 2;
  Matrix a{&input, &input, 0, {1, 1}, {1, 1}}, b{&other, &other, 0, {1, 1}, {1, 1}}, result{};
  _mlir_ciface_kalandra_test_entry(&result, &a, &b, &context);
  check(!context.status() && !context.allocations() && !context.live());
  context.settle();
#ifdef KALANDRA_MATRIX_SHARED
  std::printf("Verified %u shared-input matrix cases and allocation failures\n", cases);
#else
  std::printf("Verified %u per-cell matrix cases, tails and allocation failures\n", cases);
#endif
}
