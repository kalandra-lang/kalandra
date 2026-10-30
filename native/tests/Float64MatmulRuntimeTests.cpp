#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
using Tensor = CpuTensor<NumericType::Float64, 2>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, Invocation *);
static std::uint64_t cases = 0, faults = 0, comparisons = 0;
static void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct View {
  std::vector<double> storage;
  Tensor tensor{};
  View(std::int64_t rows, std::int64_t cols, const std::vector<Bits> &values, bool transpose)
      : storage(2 * rows * cols + 3 * (rows + cols) + 8, 42.0) {
    check(values.size() == std::size_t(rows * cols), "invalid harness shape");
    tensor.allocated = storage.data();
    tensor.aligned = storage.data() + 1;
    tensor.offset = 2;
    tensor.sizes = {rows, cols};
    tensor.strides = transpose ? std::array<std::int64_t, 2>{2, 2 * rows + 3}
                               : std::array<std::int64_t, 2>{2 * cols + 3, 2};
    for (std::int64_t r = 0; r < rows; ++r)
      for (std::int64_t c = 0; c < cols; ++c)
        tensor.aligned[tensor.offset + r * tensor.strides[0] + c * tensor.strides[1]] =
            std::bit_cast<double>(values[r * cols + c]);
  }
};
static std::vector<Bits> reference(std::int64_t rows, std::int64_t inner, std::int64_t cols,
                                   const std::vector<Bits> &a, const std::vector<Bits> &b) {
  std::vector<Bits> result;
  for (std::int64_t r = 0; r < rows; ++r)
    for (std::int64_t c = 0; c < cols; ++c) {
      std::vector<Bits> tree;
      for (std::int64_t k = 0; k < inner; ++k)
        tree.push_back(oracle({a[r * inner + k], b[k * cols + c]}, 1));
      while (tree.size() > 1) {
        std::vector<Bits> next;
        for (std::size_t i = 0; i < tree.size(); i += 2)
          next.push_back(i + 1 == tree.size() ? tree[i] : oracle({tree[i], tree[i + 1]}, 0));
        tree = std::move(next);
      }
      result.push_back(tree.empty() ? 0 : tree.front());
    }
  return result;
}
static void verify(std::int64_t rows, std::int64_t inner, std::int64_t cols,
                   const std::vector<Bits> &a, const std::vector<Bits> &b,
                   const std::vector<Bits> &expected, unsigned fail = 0, bool alias = false,
                   bool transpose = false, bool mismatch = false) {
  View left(rows, inner, a, transpose), right(inner, cols, b, true);
  const auto beforeLeft = left.storage, beforeRight = right.storage;
  if (mismatch)
    ++right.tensor.sizes[0];
  Tensor result{};
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &left.tensor, alias ? &left.tensor : &right.tensor,
                                   &context);
  const auto code = mismatch ? 1 : fail ? 2 : 0;
  ++cases;
  faults += code != 0;
  check(context.status() == code, "matmul status mismatch");
  std::optional<RetainedCpuTensor> retained;
  auto cells = [&] {
    check(result.sizes == std::array<std::int64_t, 2>{rows, cols}, "wrong output shape");
    check(bool(result.aligned), "missing output storage");
    for (std::int64_t r = 0; r < rows; ++r)
      for (std::int64_t c = 0; c < cols; ++c) {
        auto actual = std::bit_cast<Bits>(
            result.aligned[result.offset + r * result.strides[0] + c * result.strides[1]]);
        if (actual != expected[r * cols + c]) {
          std::cerr << "shape " << rows << "," << inner << "," << cols << " cell " << r << "," << c
                    << " actual " << std::hex << actual << " expected " << expected[r * cols + c]
                    << std::dec << '\n';
          throw std::runtime_error("matmul result bits mismatch");
        }
        ++comparisons;
      }
  };
  if (code)
    check(!result.allocated, "failed matmul published tensor");
  else {
    cells();
    retained = retainCpuTensor(context, result.view());
    check(bool(retained), "result lacks owned backing");
  }
  check(std::memcmp(left.storage.data(), beforeLeft.data(), left.storage.size() * sizeof(double)) ==
                0 &&
            std::memcmp(right.storage.data(), beforeRight.data(),
                        right.storage.size() * sizeof(double)) == 0,
        "borrowed input mutated");
  context.settle();
  check(!context.live() && context.allocations() == context.releases(),
        "matmul allocations leaked");
  if (!code)
    cells();
}
static void metadata(std::array<std::int64_t, 2> a, std::array<std::int64_t, 2> b, int code,
                     unsigned fail = 0) {
  double value = 1;
  Tensor x{}, y{}, out{};
  x.allocated = x.aligned = y.allocated = y.aligned = &value;
  x.sizes = a;
  y.sizes = b;
  x.strides = y.strides = {1, 1};
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&out, &x, &y, &context);
  ++cases;
  faults += code != 0;
  check(context.status() == code && (!code || !out.allocated), "invalid metadata published result");
  if (!code)
    check(out.sizes == std::array<std::int64_t, 2>{a[0], b[1]}, "wrong empty metadata shape");
  context.settle();
  check(!context.live() && context.allocations() == context.releases(), "metadata failure leaked");
}
static void cancelled() {
  std::vector<Bits> a(17 * 8193, 0x3ff0000000000000), b(8193 * 3, 0x3ff0000000000000);
  View left(17, 8193, a, false), right(8193, 3, b, true);
  Invocation context;
  std::atomic<bool> token{false}, done{false}, ready{false};
  std::atomic<std::uint64_t> polls{0};
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  std::thread signal([&] {
    ready.store(true);
    while (!done.load() && polls.load() < 4)
      std::this_thread::yield();
    if (!done.load())
      token.store(true);
  });
  while (!ready.load())
    std::this_thread::yield();
  Tensor out{};
  _mlir_ciface_kalandra_test_entry(&out, &left.tensor, &right.tensor, &context);
  done.store(true);
  signal.join();
  check(token.load() && context.poll() == -1 && !context.status() && !out.allocated,
        "matmul cancellation failed");
  context.settle();
  check(!context.live() && context.allocations() == context.releases(), "cancelled matmul leaked");
}
int main() {
  try {
    struct Batch {
      std::int64_t rows, inner, cols;
      std::vector<Bits> a, b, expected;
    };
    std::vector<Batch> batches;
    auto add = [&](std::int64_t rows, std::int64_t inner, std::int64_t cols, std::vector<Bits> a,
                   std::vector<Bits> b) {
      auto expected = reference(rows, inner, cols, a, b);
      batches.push_back({rows, inner, cols, std::move(a), std::move(b), std::move(expected)});
    };
    auto pairs = corpus();
    for (std::size_t i = 0; i < 8192; ++i) {
      auto pair = pairs[i * (pairs.size() - 1) / 8191];
      add(1, 1, 1, {pair.a}, {pair.b});
    }
    for (auto k : {0, 1, 2, 3, 4, 5, 7, 8, 9, 255, 256, 257, 1023, 1024, 1025, 32769}) {
      std::vector<Bits> a(2 * k), b(k * 3);
      for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = pairs[(i * 7919 + k) % pairs.size()].a;
      for (std::size_t i = 0; i < b.size(); ++i)
        b[i] = pairs[(i * 3571 + k) % pairs.size()].b;
      add(2, k, 3, std::move(a), std::move(b));
    }
    add(1, 2, 1, {0x3ff0000000000001, 0xbff0000000000000},
        {0x3feffffffffffffe, 0x3ff0000000000000});
    add(1, 4, 1, {0x4350000000000000, 0x3ff0000000000000, 0xc350000000000000, 0x3ff0000000000000},
        std::vector<Bits>(4, 0x3ff0000000000000));
    add(1, 3, 1, {Sign, Sign, Sign}, std::vector<Bits>(3, 0x3ff0000000000000));
    add(1, 3, 1, {1, 1, 1}, std::vector<Bits>(3, 0x3ff0000000000000));
    add(1, 3, 1, {0x7fefffffffffffff, 0x7fefffffffffffff, Sign | Infinity},
        std::vector<Bits>(3, 0x4000000000000000));
    add(0, 3, 4, {}, std::vector<Bits>(12, Infinity));
    add(3, 4, 0, std::vector<Bits>(12, Infinity), {});
    Bits seed = 0x318a92f7d5b6e4c1;
    auto next = [&] {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      return seed;
    };
    for (unsigned trial = 0; trial < 512; ++trial) {
      auto r = next() % 5, k = next() % 10, c = next() % 6;
      std::vector<Bits> a(r * k), b(k * c);
      for (auto &v : a)
        v = next();
      for (auto &v : b)
        v = next();
      add(r, k, c, std::move(a), std::move(b));
    }
    std::vector<Bits> square{0x3ff0000000000001, 1, Sign, 0x4000000000000000};
    auto squareExpected = reference(2, 2, 2, square, square);
    std::vector<Bits> nonSquare{
        0x4350000000000000, 0x3ff0000000000000, 0xc350000000000000, 0x3ff0000000000000, 1, Sign};
    auto nonSquareExpected = reference(2, 3, 2, nonSquare, nonSquare);
    {
      FpState state;
      for (unsigned environment = 0; environment < 8; ++environment) {
        state.set(environment);
        for (const auto &batch : batches)
          verify(batch.rows, batch.inner, batch.cols, batch.a, batch.b, batch.expected);
        verify(2, 2, 2, square, square, squareExpected, 0, true);
        verify(2, 3, 2, nonSquare, nonSquare, nonSquareExpected, 0, false, true);
        state.checkFlags();
      }
    }
    for (unsigned fail : {1, 2})
      verify(2, 2, 2, square, square, {}, fail);
    verify(2, 2, 2, square, square, {}, 0, false, false, true);
    metadata({-1, 1}, {1, 1}, 1);
    metadata({1, -1}, {-1, 1}, 1);
    metadata({1, 1}, {1, -1}, 1);
    metadata({0, 1}, {2, 0}, 1);
    metadata({INT64_MAX, 0}, {0, 2}, 2);
    metadata({1, INT64_MAX}, {INT64_MAX, 1}, 2);
    metadata({0, INT64_MAX}, {INT64_MAX, 0}, 0);
    metadata({0, INT64_MAX / 8}, {INT64_MAX / 8, 1}, 0);
    metadata({1, INT64_MAX / 8}, {INT64_MAX / 8, 0}, 0);
    metadata({INT64_MAX / 8 + 1, 0}, {0, 1}, 2);
    metadata({INT64_MAX / 8, 0}, {0, 1}, 2, 1);
    cancelled();
    std::cout << "float64 matmul: " << cases << " cases, " << faults << " faults, " << comparisons
              << " exact APFloat cell checks; separately rounded products, adjacent trees, eight "
                 "FP environments and settled retained owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
