#include "../runtime/Tensor.h"
#include <array>
#include <bit>
#include <iostream>
#include <string_view>
#include <vector>
#ifndef TENSOR_RANK
#define TENSOR_RANK 2
#endif
using namespace kalandra;
using namespace kalandra::runtime;
using Tensor = CpuTensor<NumericType::Int32, TENSOR_RANK>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, std::int32_t,
                                                 Invocation *);
static std::string_view operation;
static int mode;
static std::uint64_t cases = 0, faults = 0;
template <std::size_t Rank>
static bool verify(std::array<std::int64_t, Rank> shape, std::vector<std::int32_t> a,
                   std::vector<std::int32_t> b, std::int32_t scalar = 1, bool mismatch = false,
                   unsigned fail = 0, bool alias = false) {
  std::size_t count = 1;
  for (auto d : shape)
    count *= std::size_t(d);
  a.resize(count);
  b.resize(count);
  std::vector<std::int32_t> left(count + 4, 1234567), right(count + 4, -7654321);
  std::copy(a.begin(), a.end(), left.begin() + 2);
  std::copy(b.begin(), b.end(), right.begin() + 2);
  auto beforeLeft = left, beforeRight = right;
  CpuTensor<NumericType::Int32, Rank> x{}, y{}, result{};
  x.allocated = left.data();
  x.aligned = left.data() + 1;
  x.offset = 1;
  y.allocated = right.data();
  y.aligned = right.data() + 1;
  y.offset = 1;
  if constexpr (Rank > 0) {
    x.sizes = y.sizes = shape;
    std::int64_t stride = 1;
    for (std::size_t n = Rank; n > 0; --n) {
      x.strides[n - 1] = y.strides[n - 1] = stride;
      stride *= shape[n - 1];
    }
    if (mismatch)
      y.sizes[0]++;
  }
  std::vector<std::int64_t> expected;
  bool arithmetic = false;
  if (mode == 4) {
    if constexpr (Rank == 0)
      expected = {scalar};
    else
      expected = {scalar, INT32_MAX, INT32_MIN, 16777217};
  } else {
    for (std::size_t n = 0; n < count; ++n) {
      std::int64_t p = mode == 2 ? scalar : a[n], q = mode == 5   ? 1
                                                      : mode == 1 ? scalar
                                                                  : (alias ? a[n] : b[n]);
      std::int64_t value = 0;
      if (operation == "add")
        value = p + q;
      else if (operation == "sub")
        value = p - q;
      else if (operation == "mul")
        value = p * q;
      else if (operation == "div" || operation == "rem") {
        if (q == 0) {
          arithmetic = true;
          continue;
        }
        value = operation == "div" ? p / q : p % q;
      } else if (operation == "neg")
        value = -p;
      else if (operation == "pos")
        value = p;
      else
        return false;
      arithmetic |= value < INT32_MIN || value > INT32_MAX;
      expected.push_back(value);
    }
  }
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &x, alias ? &x : &y, scalar, &context);
  ++cases;
  const bool refinement =
      mode == 5 && std::any_of(shape.begin(), shape.end(), [](auto d) { return d != 2; });
  std::int64_t expectedCode = (mismatch || refinement) ? 1 : fail ? 2 : arithmetic ? 4 : 0;
  if (expectedCode)
    ++faults;
  auto reject = [&](const char *why) {
    std::cerr << why << ": " << operation << " mode " << mode << " rank " << Rank << " count "
              << count << " status " << context.status() << " expected " << expectedCode << '\n';
    return false;
  };
  if (context.status() != expectedCode)
    return reject("fault mismatch");
  if (expectedCode) {
    if (result.allocated)
      return reject("partial result published");
    if (expectedCode == 4 && (context.line() != 2 || context.column() != 3))
      return reject("fault location lost");
  } else {
    if constexpr (Rank > 0) {
      auto resultShape = shape;
      if (mode == 4) {
        resultShape.fill(1);
        if constexpr (Rank == 1)
          resultShape[0] = 4;
        if constexpr (Rank >= 2) {
          resultShape[0] = 2;
          resultShape[1] = 2;
        }
      }
      if (result.sizes != resultShape)
        return reject("wrong result shape");
    }
    for (std::size_t n = 0; n < expected.size(); ++n) {
      std::int64_t offset = result.offset;
      auto flat = n;
      if constexpr (Rank > 0)
        for (std::size_t d = Rank; d > 0; --d) {
          offset += std::int64_t(flat % std::size_t(result.sizes[d - 1])) * result.strides[d - 1];
          flat /= std::size_t(result.sizes[d - 1]);
        }
      if (!result.aligned || result.aligned[offset] != expected[n])
        return reject("wrong integer cell");
    }
  }
  if (left != beforeLeft || right != beforeRight)
    return reject("input mutated");
  context.settle();
  if (context.live() || context.allocations() != context.releases())
    return reject("owners not settled");
  return true;
}
template <std::size_t Rank>
static bool invalidMetadata(std::array<std::int64_t, Rank> shape, std::int64_t code) {
  CpuTensor<NumericType::Int32, Rank> input{}, result{};
  std::int32_t storage = 1;
  input.allocated = input.aligned = &storage;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    input.strides.fill(1);
  }
  Invocation context;
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, 1, &context);
  ++cases;
  ++faults;
  if (context.status() != code || result.allocated)
    return false;
  context.settle();
  return !context.live() && context.allocations() == context.releases();
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  operation = argv[1];
  mode = argv[2][0] - '0';
  std::array<std::int64_t, TENSOR_RANK> shape{};
  shape.fill(1);
  if (mode == 4) {
    if (!verify(shape, {1}, {1}, 16777219))
      return 1;
  } else if (mode == 5) {
    shape.fill(2);
    if (!verify(shape, {1, 2, 3, 4}, {1, 1, 1, 1}, 1) ||
        !verify(shape, {1, 2, 3, INT32_MAX}, {1, 1, 1, 1}, 1) ||
        !verify(shape, {1, 2, 3, 4}, {1, 1, 1, 1}, 1, false, 1))
      return 1;
    shape.fill(1);
    if (!verify(shape, {1}, {1}, 1))
      return 1;
  } else {
    constexpr std::array<std::int32_t, 19> edges{INT32_MIN,   INT32_MIN + 1,
                                                 -1073741824, -65536,
                                                 -46341,      -46340,
                                                 -3,          -2,
                                                 -1,          0,
                                                 1,           2,
                                                 3,           46340,
                                                 46341,       65536,
                                                 1073741824,  INT32_MAX - 1,
                                                 INT32_MAX};
    for (auto a : edges)
      for (auto b : edges)
        if (!verify(shape, {a}, {b}, mode == 2 ? a : b))
          return 1;
    std::uint32_t state = 0x12345678;
    auto next = [&] {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return std::bit_cast<std::int32_t>(state);
    };
    for (int n = 0; n < 1000; ++n) {
      auto a = next(), b = next();
      if (!verify(shape, {a}, {b}, mode == 2 ? a : b))
        return 1;
    }
    if constexpr (TENSOR_RANK > 0) {
      for (auto n : {0, 1, 3, 17}) {
        shape.fill(1);
        shape[0] = n;
        std::vector<std::int32_t> a(n, 6), b(n, 2);
        if (!verify(shape, a, b, 2) || !verify(shape, a, b, 2, false, 0, true))
          return 1;
        if (mode == 0 && !verify(shape, a, b, 2, true))
          return 1;
      }
      shape.fill(2);
      std::size_t full = std::size_t(1) << TENSOR_RANK;
      std::vector<std::int32_t> denseA(full), denseB(full, 2);
      for (std::size_t i = 0; i < full; ++i)
        denseA[i] = std::int32_t(i) - 3;
      if (!verify(shape, denseA, denseB, 2))
        return 1;
      for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
        auto empty = shape;
        empty[axis] = 0;
        if (!verify(empty, {}, {}, 0))
          return 1;
      }
      auto invalid = shape;
      invalid[0] = -1;
      if (!invalidMetadata(invalid, 1))
        return 1;
      invalid = shape;
      invalid[0] = INT64_MAX;
      if (!invalidMetadata(invalid, 2))
        return 1;
      // A late failing lane must preserve inputs and publish no partial tensor.
      if (operation != "pos") {
        shape.fill(1);
        shape[0] = 17;
        std::vector<std::int32_t> a(17, 6), b(17, 2);
        std::int32_t scalar = 2;
        if (operation == "add") {
          a.back() = INT32_MAX;
          b.back() = INT32_MAX;
          scalar = 1;
        }
        if (operation == "sub") {
          a.back() = INT32_MIN;
          b.back() = mode == 2 ? -1 : 1;
          scalar = mode == 2 ? INT32_MAX : 1;
        }
        if (operation == "mul") {
          a.back() = INT32_MAX;
          b.back() = INT32_MAX;
          scalar = 2;
        }
        if (operation == "div" || operation == "rem") {
          a.back() = 0;
          b.back() = 0;
          scalar = 0;
        }
        if (operation == "neg")
          a.back() = INT32_MIN;
        if (!verify(shape, a, b, scalar))
          return 1;
      }
    }
    shape.fill(1);
    if (!verify(shape, {6}, {2}, 2, false, 1))
      return 1;
  }
  std::cout << "Int32 tensor " << operation << " mode " << mode << " rank " << TENSOR_RANK << ": "
            << cases << " cases, " << faults << " faults; inputs immutable and owners settled\n";
}
