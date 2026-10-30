#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Input = CpuTensor<element, 1>;
using Output = CpuTensor<element, RESHAPE_RANK>;
#ifndef DIMENSION_KIND
#define DIMENSION_KIND 0
#endif
#if DIMENSION_KIND == 1
using D0 = std::uint64_t;
using D1 = std::uint64_t;
#elif DIMENSION_KIND == 2
using D0 = std::int32_t;
using D1 = std::uint32_t;
#else
using D0 = std::int64_t;
using D1 = std::int64_t;
#endif
extern "C" void _mlir_ciface_kalandra_test_entry(Output *, Input *
#if RESHAPE_RANK > 0
                                                 ,
                                                 D0
#endif
#if RESHAPE_RANK > 1
                                                 ,
                                                 D1
#endif
#if RESHAPE_RANK > 2
                                                 ,
                                                 D0
#endif
                                                 ,
                                                 Invocation *);
using Shape = std::array<std::int64_t, RESHAPE_RANK>;
static std::uint64_t cases = 0, lanes = 0;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void call(Output &output, Input &input, const Shape &shape, Invocation &context) {
  (void)shape;
  _mlir_ciface_kalandra_test_entry(&output, &input
#if RESHAPE_RANK > 0
                                   ,
                                   D0(shape[0])
#endif
#if RESHAPE_RANK > 1
                                       ,
                                   D1(shape[1])
#endif
#if RESHAPE_RANK > 2
                                       ,
                                   D0(shape[2])
#endif
                                       ,
                                   &context);
}
static void verify(Shape shape, std::size_t length, int error = 0) {
  std::vector<Cell> backing(2 * length + 8);
  std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
  for (std::size_t i = 0; i < backing.size(); ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    Raw bits = Raw(seed);
    if (i < 16) {
      const Raw edges[] = {0,
                           Raw(Raw(1) << (sizeof(Cell) * 8 - 1)),
                           Raw(1),
                           Raw(~Raw(0)),
                           Raw(sizeof(Cell) == 8 ? 0x7ff0000000000000ULL : 0x7f800000ULL),
                           Raw(sizeof(Cell) == 8 ? 0x7ff0000000000001ULL : 0x7f800001ULL),
                           Raw(sizeof(Cell) == 8 ? 0x7ff8000000000001ULL : 0x7fc00001ULL)};
      bits = edges[i % std::size(edges)];
    }
    backing[i] = std::bit_cast<Cell>(bits);
  }
  const auto snapshot = backing;
  Input input{};
  input.allocated = backing.data();
  input.aligned = backing.data() + 1;
  input.offset = 2;
  input.sizes = {std::int64_t(length)};
  input.strides = {2};
  std::uint64_t allocations = 0;
  for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
    Invocation context;
    context.failAllocationAt(fail);
    Output output{};
    call(output, input, shape, context);
    ++cases;
    const int expected = error ? error : fail && fail <= allocations ? 2 : 0;
    require(context.status() == expected, "reshape error/allocation status changed");
    require(!std::memcmp(backing.data(), snapshot.data(), backing.size() * sizeof(Cell)),
            "reshape mutated borrowed input");
    auto check = [&] {
#if RESHAPE_RANK > 0
      for (std::size_t axis = 0; axis < shape.size(); ++axis)
        require(output.sizes[axis] == shape[axis], "reshape output dimensions changed");
#endif
      for (std::size_t i = 0; i < length; ++i) {
        auto at = output.offset;
#if RESHAPE_RANK > 0
        auto remaining = i;
        for (std::size_t axis = shape.size(); axis > 0; --axis) {
          at += std::int64_t(remaining % std::size_t(shape[axis - 1])) * output.strides[axis - 1];
          remaining /= std::size_t(shape[axis - 1]);
        }
#endif
        require(std::bit_cast<Raw>(output.aligned[at]) == std::bit_cast<Raw>(snapshot[3 + 2 * i]),
                "reshape changed bits or row-major order");
        ++lanes;
      }
    };
    std::optional<AllocationLease> retained;
    if (expected) {
      require(!output.allocated && !output.aligned, "failed reshape published partial output");
      if (error) {
        require(context.line() == 2 && context.column() == 3,
                "reshape failure lost original location");
        require(!context.allocations(), "reshape validation happened after allocation");
      }
    } else {
      check();
      if (length) {
        retained = context.lease(output.allocated);
        require(bool(retained), "reshape output not owned");
        std::fill(backing.begin(), backing.end(), Cell{});
        check();
        std::memcpy(backing.data(), snapshot.data(), backing.size() * sizeof(Cell));
      }
    }
    if (!fail) {
      allocations = context.allocations();
      require(allocations < 16, "unbounded reshape allocation count");
    }
    context.settle();
    require(!context.live() && context.allocations() == context.releases(),
            "reshape leaked owners");
    if (!expected)
      check();
  }
}
#if RESHAPE_RANK == 2 && DIMENSION_KIND == 0 && !defined(FIXED_RESHAPE)
static void cancellation(bool active) {
  std::vector<Cell> values(active ? 1048576 : 1, Cell{7});
  Input input{};
  input.allocated = input.aligned = values.data();
  input.sizes = {std::int64_t(values.size())};
  input.strides = {1};
  Output output{};
  Shape shape = active ? Shape{1024, 1024} : Shape{1, 1};
  Invocation context;
  std::atomic<bool> token{!active}, done{false};
  std::atomic<std::uint64_t> polls{0};
  context.bindCancellation(&token);
  context.watchPolls(&polls);
  std::thread sender;
  if (active)
    sender = std::thread([&] {
      while (!done.load() && polls.load() < 128)
        std::this_thread::yield();
      if (!done.load())
        token.store(true);
    });
  call(output, input, shape, context);
  done.store(true);
  if (active)
    sender.join();
  require(token.load() && context.poll() == -1 && !context.status() && !output.allocated &&
              !output.aligned,
          "reshape cancellation published output");
  if (active)
    require(context.allocations() && polls.load() >= 128, "reshape copy not entered");
  for (auto cell : values)
    require(cell == Cell{7}, "cancelled reshape mutated input");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "cancelled reshape leaked owners");
  ++cases;
}
#endif
int main() {
  try {
    FpState fp;
    for (unsigned environment = 0; environment < 8; ++environment) {
      fp.set(environment);
#ifdef FIXED_RESHAPE
      verify({2, 2}, 4);
      verify({1, 4}, 4, 1);
      verify({2, 3}, 6, 1);
#elif RESHAPE_RANK == 0
      verify({}, 1);
      verify({}, 0, 1);
      verify({}, 2, 1);
#elif RESHAPE_RANK == 1
      for (std::size_t n : {0u, 1u, 17u, 4096u})
        verify({std::int64_t(n)}, n);
      verify({3}, 2, 1);
      verify({-1}, 0, 1);
#elif RESHAPE_RANK == 2
      for (auto shape : {Shape{0, 0}, Shape{0, 4}, Shape{4, 0}, Shape{1, 1}, Shape{2, 3},
                         Shape{3, 2}, Shape{17, 19}})
        verify(shape, std::size_t(shape[0] * shape[1]));
      verify({2, 3}, 4, 1);
      verify({-1, 2}, 0, 1);
      if constexpr (sizeof(D0) == 8) {
        verify({INT64_MAX, 2}, 0, 2);
        verify({INT64_MAX / std::int64_t(sizeof(Cell)) + 1, 1}, 0, 2);
        verify({0, INT64_MAX}, 0);
      }
      if constexpr (DIMENSION_KIND == 2)
        verify({0, UINT32_MAX}, 0);
      if constexpr (DIMENSION_KIND == 1)
        verify({-1, 0}, 0, 1);
#elif RESHAPE_RANK == 3
      for (auto shape : {Shape{0, 2, 3}, Shape{2, 0, 3}, Shape{2, 3, 0}, Shape{1, 1, 1},
                         Shape{2, 3, 4}, Shape{3, 2, 5}})
        verify(shape, std::size_t(shape[0] * shape[1] * shape[2]));
      verify({2, 3, 4}, 23, 1);
      verify({-1, 0, 2}, 0, 1);
      verify({0, INT64_MAX, INT64_MAX}, 0, 2);
      verify({INT64_MAX, INT64_MAX, 0}, 0);
#endif
      fp.checkFlags();
    }
#if RESHAPE_RANK == 2 && DIMENSION_KIND == 0 && !defined(FIXED_RESHAPE)
    cancellation(false);
    cancellation(true);
#endif
    std::cout << "reshape " << describeNumeric(element)->name << " rank " << RESHAPE_RANK
              << " dimensions " << DIMENSION_KIND << ": " << cases << " cases, " << lanes
              << " exact lanes; checked shapes/bytes/strides, immutable borrowed input and "
                 "retained ownership passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
