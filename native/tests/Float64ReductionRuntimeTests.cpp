#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
#ifndef TENSOR_RANK
#define TENSOR_RANK 1
#endif
using Tensor = CpuTensor<NumericType::Float64, TENSOR_RANK>;
using Shape = std::array<std::int64_t, TENSOR_RANK>;
extern "C" double _mlir_ciface_kalandra_test_entry(Tensor *, Invocation *);
static std::uint64_t cases = 0, faults = 0;
static void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static Bits reference(std::vector<Bits> tree) {
  while (tree.size() > 1) {
    std::vector<Bits> next;
    for (std::size_t i = 0; i < tree.size(); i += 2)
      next.push_back(i + 1 == tree.size() ? tree[i] : oracle({tree[i], tree[i + 1]}, 0));
    tree = std::move(next);
  }
  return tree.empty() ? 0 : tree.front();
}
template <std::size_t Rank>
static std::int64_t offset(const CpuTensor<NumericType::Float64, Rank> &input, std::size_t flat) {
  auto value = input.offset;
  if constexpr (Rank > 0)
    for (std::size_t axis = Rank; axis > 0; --axis) {
      value += std::int64_t(flat % input.sizes[axis - 1]) * input.strides[axis - 1];
      flat /= input.sizes[axis - 1];
    }
  return value;
}
template <std::size_t Rank = TENSOR_RANK>
static void verify(const std::vector<Bits> &values, Shape shape, Bits expected, unsigned fail = 0,
                   int metadataFault = 0, bool transpose = false) {
  std::vector<double> storage(values.size() * 2 + 8, 42.0);
  CpuTensor<NumericType::Float64, Rank> input{};
  input.allocated = storage.data();
  input.aligned = input.allocated + 1;
  input.offset = 2;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t axis = Rank; axis > 0; --axis) {
      input.strides[axis - 1] = stride;
      if (!metadataFault && !std::ranges::contains(shape, 0))
        stride *= shape[axis - 1];
    }
    if constexpr (Rank == 2)
      if (transpose) {
        input.strides[0] = 2;
        input.strides[1] = 2 * shape[0];
      }
  }
  for (std::size_t i = 0; i < values.size(); ++i)
    storage[1 + offset(input, i)] = std::bit_cast<double>(values[i]);
  const auto before = storage;
  Invocation context;
  context.failAllocationAt(fail);
  const auto result = std::bit_cast<Bits>(_mlir_ciface_kalandra_test_entry(&input, &context));
  const auto code = metadataFault ? metadataFault : fail ? 2 : 0;
  ++cases;
  faults += code != 0;
  if (context.status() != code || result != (code ? 0 : expected)) {
    std::cerr << "rank " << TENSOR_RANK << " case " << cases << " status " << context.status()
              << " expected status " << code << " bits " << std::hex << result << " expected "
              << (code ? 0 : expected) << std::dec << '\n';
    throw std::runtime_error("reduction result mismatch");
  }
  check(std::memcmp(storage.data(), before.data(), storage.size() * sizeof(double)) == 0,
        "reduction changed borrowed input");
  context.settle();
  check(!context.live() && context.allocations() == context.releases(),
        "scratch allocation leaked");
  check(result == (code ? 0 : expected), "scalar result changed after settlement");
}
template <std::size_t Rank = TENSOR_RANK> static void cancelled() {
  const auto count = Rank == 0 ? 1 : 262144;
  std::vector<double> values(count, 1);
  CpuTensor<NumericType::Float64, Rank> input{};
  input.allocated = input.aligned = values.data();
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.sizes.back() = count;
    input.strides.fill(count);
    input.strides.back() = 1;
  }
  Invocation context;
  std::atomic<bool> token{Rank == 0}, done{false}, ready{false};
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
  auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
  done.store(true);
  signal.join();
  check(token.load() && std::bit_cast<Bits>(result) == 0 && context.poll() == -1 &&
            !context.status(),
        "reduction failed to cooperate with cancellation");
  context.settle();
  check(!context.live() && context.allocations() == context.releases(), "cancelled scratch leaked");
}
int main() {
  try {
    struct Batch {
      Shape shape;
      std::vector<Bits> values;
      Bits expected;
    };
    std::vector<Batch> batches;
    Shape shape{};
    shape.fill(1);
    auto add = [&](std::vector<Bits> values, Shape dimensions) {
      auto expected = reference(values);
      batches.push_back({dimensions, std::move(values), expected});
    };
    for (Bits bits :
         {Bits{0}, Sign, Bits{1}, Sign | 1, Bits{0x3ff0000000000001}, Bits{0x7fefffffffffffff},
          Infinity, Infinity | Sign, Bits{0x7ff0000000000001}, Bits{0xfff8000000001234}})
      add({bits}, shape);
    if constexpr (TENSOR_RANK > 0) {
      auto pairs = corpus();
      for (std::size_t i = 0; i < 8192; ++i) {
        auto p = pairs[i * (pairs.size() - 1) / 8191];
        shape.fill(1);
        shape.back() = 2;
        add({p.a, p.b}, shape);
      }
      for (auto size : {0, 1, 2, 3, 4, 5, 7, 8, 9, 255, 256, 257, 1023, 1024, 1025, 32769}) {
        shape.fill(1);
        shape.back() = size;
        std::vector<Bits> values(size);
        for (int i = 0; i < size; ++i)
          values[i] = pairs[(std::size_t(i) * 7919 + size) % pairs.size()].a;
        add(values, shape);
        std::fill(values.begin(), values.end(), Sign);
        add(values, shape);
      }
      for (auto values :
           {std::vector<Bits>{0x4350000000000000, 0x3ff0000000000000, 0xc350000000000000,
                              0x3ff0000000000000},
            std::vector<Bits>{0x7fefffffffffffff, 0x7fefffffffffffff, 0xffefffffffffffff},
            std::vector<Bits>{Sign, Sign, Sign}, std::vector<Bits>{1, 1, 1, 1, 1},
            std::vector<Bits>{Infinity, Infinity | Sign, 0x7ff0000000000001}}) {
        shape.fill(1);
        shape.back() = values.size();
        add(values, shape);
      }
      for (std::size_t axis = 0; axis < TENSOR_RANK; ++axis) {
        shape.fill(3);
        shape[axis] = 0;
        add({}, shape);
      }
      if constexpr (TENSOR_RANK == 2) {
        shape.fill(1);
        shape[0] = 2;
        shape[1] = 3;
        add({0x4350000000000000, 0x3ff0000000000000, 0xc350000000000000, 0x3ff0000000000000, 1,
             Sign},
            shape);
      }
    }
    {
      FpState state;
      for (unsigned environment = 0; environment < 8; ++environment) {
        state.set(environment);
        for (const auto &batch : batches)
          verify(batch.values, batch.shape, batch.expected);
        if constexpr (TENSOR_RANK == 2) {
          auto &batch = batches.back();
          verify(batch.values, batch.shape, batch.expected, 0, 0, true);
        }
        state.checkFlags();
      }
    }
    shape.fill(1);
    verify({1}, shape, 1, 1);
    if constexpr (TENSOR_RANK > 0) {
      shape[0] = -1;
      verify({}, shape, 0, 0, 1);
      shape[0] = INT64_MAX / 8 + 1;
      verify({}, shape, 0, 0, 2);
      shape[0] = INT64_MAX / 8;
      verify({}, shape, 0, 1, 2);
    }
    cancelled();
    std::cout << "float64 reduceSum rank " << TENSOR_RANK << ": " << cases << " cases, " << faults
              << " faults; exact APFloat adjacent tree, eight FP environments, immutable views and "
                 "settled scratch passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
