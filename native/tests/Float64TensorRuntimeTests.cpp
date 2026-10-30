#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#ifndef TENSOR_RANK
#define TENSOR_RANK 2
#endif
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
using Tensor = CpuTensor<NumericType::Float64, TENSOR_RANK>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, double,
                                                 Invocation *);
static unsigned operation, mode;
static std::uint64_t cases = 0, faults = 0, comparisons = 0;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
using Shape = std::array<std::int64_t, TENSOR_RANK>;
static std::size_t count(const Shape &shape) {
  std::size_t size = 1;
  for (auto extent : shape)
    size *= extent;
  return size;
}
template <std::size_t Rank>
static std::int64_t offset(const CpuTensor<NumericType::Float64, Rank> &tensor, std::size_t flat) {
  auto value = tensor.offset;
  if constexpr (Rank > 0)
    for (std::size_t axis = Rank; axis > 0; --axis) {
      value += std::int64_t(flat % tensor.sizes[axis - 1]) * tensor.strides[axis - 1];
      flat /= tensor.sizes[axis - 1];
    }
  return value;
}
template <std::size_t Rank>
static void layout(CpuTensor<NumericType::Float64, Rank> &tensor, const Shape &shape,
                   bool transpose) {
  if constexpr (Rank > 0) {
    tensor.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t axis = Rank; axis > 0; --axis) {
      tensor.strides[axis - 1] = stride;
      stride *= shape[axis - 1];
    }
    if constexpr (Rank == 2)
      if (transpose) {
        tensor.strides[0] = 2;
        tensor.strides[1] = 2 * shape[0];
      }
  }
}
static std::vector<Bits> reference(const std::vector<Bits> &a, const std::vector<Bits> &b,
                                   Bits scalar) {
  if (mode == 7)
    return {};
  if (mode == 4) {
    if constexpr (TENSOR_RANK == 0)
      return {scalar};
    return {scalar, 0x3ff0000000000001, 1, 0x7fefffffffffffff};
  }
  std::vector<Bits> result;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto p = mode == 2 ? scalar : a[i], q = mode == 5   ? Bits{0x3ff0000000000000}
                                            : mode == 1 ? scalar
                                                        : b[i];
    result.push_back(operation == 6 ? p : oracle({p, q}, operation));
  }
  return result;
}
template <std::size_t Rank = TENSOR_RANK>
static void verify(Shape shape, const std::vector<Bits> &a, const std::vector<Bits> &b, Bits scalar,
                   const std::vector<Bits> &expected, bool mismatch = false, unsigned fail = 0,
                   bool alias = false, bool transpose = false) {
  const auto cells = count(shape);
  require(a.size() == cells && b.size() == cells, "invalid harness inputs");
  std::vector<double> left(2 * cells + 8, std::bit_cast<double>(Bits{0x4045000000000000})),
      right(2 * cells + 8, std::bit_cast<double>(Bits{0xc045000000000000}));
  CpuTensor<NumericType::Float64, Rank> x{}, y{}, result{};
  x.allocated = left.data();
  x.aligned = left.data() + 1;
  x.offset = 2;
  y.allocated = right.data();
  y.aligned = right.data() + 1;
  y.offset = 2;
  layout(x, shape, transpose);
  layout(y, shape, false);
  for (std::size_t i = 0; i < cells; ++i) {
    x.aligned[offset(x, i)] = std::bit_cast<double>(a[i]);
    y.aligned[offset(y, i)] = std::bit_cast<double>(b[i]);
  }
  const auto beforeLeft = left, beforeRight = right;
  if constexpr (Rank > 0)
    if (mismatch)
      ++y.sizes[0];
  const bool refinement =
      mode == 5 && std::any_of(shape.begin(), shape.end(), [](auto d) { return d != 2; });
  const auto code = mismatch || refinement ? 1 : fail ? 2 : 0;
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &x, alias ? &x : &y, std::bit_cast<double>(scalar),
                                   &context);
  ++cases;
  faults += code != 0;
  require(context.status() == code, "tensor status mismatch");
  auto correct = [&] {
    auto want = shape;
    if (mode == 7) {
      if constexpr (Rank > 0)
        want[0] = 0;
      if constexpr (Rank == 2)
        want[1] = 3;
    }
    if (mode == 4) {
      want.fill(1);
      if constexpr (Rank == 1)
        want[0] = 4;
      if constexpr (Rank >= 2) {
        want[0] = 2;
        want[1] = 2;
      }
    }
    if constexpr (Rank > 0)
      require(result.sizes == want, "tensor result shape mismatch");
    require(bool(result.aligned), "tensor result has no backing");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto actual = std::bit_cast<Bits>(result.aligned[offset(result, i)]);
      if (actual != expected[i]) {
        std::cerr << "op " << operation << " mode " << mode << " rank " << Rank << " cell " << i
                  << " expected " << std::hex << expected[i] << " actual " << actual << std::dec
                  << '\n';
        throw std::runtime_error("tensor result bits mismatch");
      }
      ++comparisons;
    }
  };
  std::optional<AllocationLease> retained;
  if (code)
    require(!result.allocated, "partial tensor result published after fault");
  else {
    correct();
    retained = context.lease(result.allocated);
    require(bool(retained), "tensor result is not invocation owned");
  }
  require(!std::memcmp(left.data(), beforeLeft.data(), left.size() * sizeof(double)) &&
              !std::memcmp(right.data(), beforeRight.data(), right.size() * sizeof(double)),
          "borrowed tensor input mutated");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(), "tensor owners leaked");
  if (!code)
    correct(); // The retained backing survives invocation settlement.
}
template <std::size_t Rank = TENSOR_RANK>
static void metadata(Shape shape, int code, unsigned fail = 0, bool allocationReached = false) {
  double cell = 1;
  CpuTensor<NumericType::Float64, Rank> input{}, result{};
  input.allocated = input.aligned = &cell;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    input.strides.fill(1);
  }
  Invocation context;
  context.failAllocationAt(fail);
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, 1, &context);
  ++cases;
  ++faults;
  require(context.status() == code && !result.allocated, "invalid tensor metadata published data");
  if (allocationReached)
    require(context.line() == 0, "exact byte bound failed before allocation");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "metadata failure leaked owners");
}
template <std::size_t Rank = TENSOR_RANK>
static std::optional<bool> cancellationAttempt(std::size_t size) {
  std::vector<double> cells(size, 0);
  CpuTensor<NumericType::Float64, Rank> input{}, result{};
  input.allocated = input.aligned = cells.data();
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = size;
    input.strides.fill(size);
    input.strides.back() = 1;
  }
  Invocation context;
  std::atomic<bool> token{Rank == 0}, ready{false}, done{false};
  std::atomic<std::uint64_t> polls{0};
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  std::thread sender([&] {
    ready.store(true);
    while (!done.load() && polls.load() < 4)
      std::this_thread::yield();
    if (!done.load())
      token.store(true);
  });
  while (!ready.load())
    std::this_thread::yield();
  _mlir_ciface_kalandra_test_entry(&result, &input, &input, 0, &context);
  done.store(true);
  sender.join();
  const bool completed = result.allocated && !context.status();
  const bool stopped =
      token.load() && context.poll() == -1 && !context.status() && !result.allocated;
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "cancelled tensor leaked owners");
  if (completed)
    return {};
  return stopped;
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  operation = std::strtoul(argv[1], nullptr, 10);
  mode = std::strtoul(argv[2], nullptr, 10);
  try {
    Shape shape;
    shape.fill(1);
    if (mode == 4 || mode == 5 || mode == 7) {
      if (mode == 5)
        shape.fill(2);
      std::vector<Bits> a(count(shape), 0x3ff0000000000001), b(count(shape), 0);
      auto expected = reference(a, b, Sign);
      FpState state;
      for (unsigned environment = 0; environment < 8; ++environment) {
        state.set(environment);
        verify(shape, a, b, Sign, expected);
        verify(shape, a, b, Sign, expected, false, 1);
        if (mode == 5) {
          auto invalid = shape;
          invalid.fill(1);
          verify(invalid, {0}, {0}, 0, {});
        }
        state.checkFlags();
      }
    } else {
      const auto pairs = corpus();
      // Precompute APFloat expectations before installing hostile FP controls.
      struct Batch {
        Shape shape;
        std::vector<Bits> a, b, expected;
        Bits scalar;
      };
      std::vector<Batch> batches;
      const auto block = TENSOR_RANK == 0 ? 1 : 512;
      // Rank zero uses a bounded subset; full ranked proofs share the entire scalar corpus.
      const auto total =
          TENSOR_RANK == 0 ? std::min<std::size_t>(pairs.size(), 4096) : pairs.size();
      for (std::size_t start = 0; start < total; start += block) {
        const auto length = std::min<std::size_t>(block, total - start);
        Batch batch;
        batch.shape.fill(1);
        if constexpr (TENSOR_RANK > 0)
          batch.shape.back() = length;
        if constexpr (TENSOR_RANK == 2)
          if (length == 512) {
            batch.shape[0] = 16;
            batch.shape[1] = 32;
          }
        batch.scalar = mode == 2 ? pairs[TENSOR_RANK == 0 ? start * pairs.size() / total : start].a
                                 : pairs[TENSOR_RANK == 0 ? start * pairs.size() / total : start].b;
        for (std::size_t i = 0; i < length; ++i) {
          batch.a.push_back(
              pairs[TENSOR_RANK == 0 ? (start + i) * pairs.size() / total : start + i].a);
          batch.b.push_back(
              pairs[TENSOR_RANK == 0 ? (start + i) * pairs.size() / total : start + i].b);
        }
        batch.expected = reference(batch.a, batch.b, batch.scalar);
        batches.push_back(std::move(batch));
      }
      {
        FpState state;
        for (unsigned environment = 0; environment < 8; ++environment) {
          state.set(environment);
          for (const auto &batch : batches)
            verify(batch.shape, batch.a, batch.b, batch.scalar, batch.expected);
          state.checkFlags();
        }
      }
      shape.fill(1);
      verify(shape, {0x3ff0000000000001}, {0x4000000000000000}, 0x4000000000000000, {}, false, 1);
      // Composed elementwise expressions allocate multiple intermediates.
      if (operation == 2) {
        auto expected = reference({0x3ff0000000000001}, {0x4000000000000000}, 0);
        verify(shape, {0x3ff0000000000001}, {0x4000000000000000}, 0, expected, false, 2);
      }
      if constexpr (TENSOR_RANK > 0) {
        shape.fill(2);
        if constexpr (TENSOR_RANK == 2)
          shape[1] = 3;
        auto a = std::vector<Bits>(count(shape)), b = std::vector<Bits>(count(shape));
        for (std::size_t i = 0; i < a.size(); ++i) {
          a[i] = 0x3ff0000000000000 + 3 * i;
          b[i] = 0x4000000000000000 + 5 * i;
        }
        verify(shape, a, b, 0x4000000000000000, reference(a, b, 0x4000000000000000), false, 0,
               false, true);
        verify(shape, a, a, 0, reference(a, a, 0), false, 0, true);
        if (mode == 0)
          verify(shape, a, b, 0, {}, true);
        for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
          auto empty = shape;
          empty[axis] = 0;
          verify(empty, {}, {}, 0, {});
        }
        auto invalid = shape;
        invalid[0] = -1;
        metadata(invalid, 1);
        invalid.fill(1);
        invalid[0] = INT64_MAX / 8 + 1;
        metadata(invalid, 2);
        invalid[0] = INT64_MAX;
        metadata(invalid, 2);
        invalid[0] = INT64_MAX / 8;
        metadata(invalid, 2, 1, true);
      }
      if (operation == 0 && mode == 0) {
        bool observed = false;
        for (auto size : {65536, 262144, 1048576})
          if (auto stopped = cancellationAttempt(size)) {
            require(*stopped, "tensor cancellation did not settle");
            observed = true;
            break;
          }
        require(observed, "tensor cancellation completed before observing every probe");
      }
    }
    std::cout << "float64 tensor op " << operation << " mode " << mode << " rank " << TENSOR_RANK
              << ": " << cases << " cases, " << faults << " faults, " << comparisons
              << " exact APFloat/identity lane checks; immutable views, eight FP environments and "
                 "settled retained owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
