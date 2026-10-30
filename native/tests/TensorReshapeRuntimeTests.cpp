#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Input = CpuTensor<element, INPUT_RANK>;
using Output = CpuTensor<element, OUTPUT_RANK>;
using InputShape = std::array<std::int64_t, INPUT_RANK>;
using OutputShape = std::array<std::int64_t, OUTPUT_RANK>;
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
#if OUTPUT_RANK > 0 && !defined(TENSOR_TO_ARRAY)
                                                 ,
                                                 D0
#endif
#if OUTPUT_RANK > 1 && !defined(TENSOR_TO_ARRAY)
                                                 ,
                                                 D1
#endif
#if OUTPUT_RANK > 2 && !defined(TENSOR_TO_ARRAY)
                                                 ,
                                                 D0
#endif
                                                 ,
                                                 Invocation *);
static void invoke(Output &output, Input &input, const OutputShape &shape, Invocation &context) {
  (void)shape;
  _mlir_ciface_kalandra_test_entry(&output, &input
#if OUTPUT_RANK > 0 && !defined(TENSOR_TO_ARRAY)
                                   ,
                                   D0(shape[0])
#endif
#if OUTPUT_RANK > 1 && !defined(TENSOR_TO_ARRAY)
                                       ,
                                   D1(shape[1])
#endif
#if OUTPUT_RANK > 2 && !defined(TENSOR_TO_ARRAY)
                                       ,
                                   D0(shape[2])
#endif
                                       ,
                                   &context);
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::uint64_t cases = 0, lanes = 0;
template <std::size_t Rank>
static std::int64_t physical(const CpuTensor<element, Rank> &view, std::size_t flat,
                             const std::array<std::int64_t, Rank> &shape) {
  auto at = view.offset;
  if constexpr (Rank > 0)
    for (std::size_t axis = Rank; axis-- > 0;) {
      at += std::int64_t(flat % std::size_t(shape[axis])) * view.strides[axis];
      flat /= std::size_t(shape[axis]);
    }
  else {
    (void)flat;
    (void)shape;
  }
  return at;
}
template <std::size_t Rank>
static CpuTensor<element, Rank> descriptor(Cell *data, std::array<std::int64_t, Rank> shape,
                                           unsigned layout) {
  CpuTensor<element, Rank> input{};
  input.allocated = data;
  input.aligned = data + 1;
  input.offset = 2;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t i = 0; i < Rank; ++i) {
      auto axis = layout == 1 ? i : Rank - i - 1;
      input.strides[axis] = layout == 2 ? -stride : stride;
      if (layout == 2 && shape[axis])
        input.offset += (shape[axis] - 1) * stride;
      stride *= shape[axis] ? shape[axis] : 1;
    }
  }
  return input;
}
static void verify(InputShape inputShape, OutputShape outputShape, unsigned layout, int error = 0) {
  std::size_t count = 1;
  for (auto extent : inputShape)
    count *= std::size_t(extent);
  std::vector<Cell> cells(2 * count + 32);
  for (std::size_t i = 0; i < cells.size(); ++i) {
    Raw bits = Raw(0xf17deacb1297ff83ULL) * Raw(i + 1);
    const Raw edges[] = {0,
                         Raw(Raw(1) << (8 * sizeof(Cell) - 1)),
                         1,
                         Raw(-1),
                         sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                         sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL),
                         sizeof(Cell) == 4 ? Raw(0x7fc12345) : Raw(0x7ff812345678abcdULL)};
    if (i < 16)
      bits = edges[i % 7];
    cells[i] = std::bit_cast<Cell>(bits);
  }
  const auto before = cells;
  auto input = descriptor<INPUT_RANK>(cells.data(), inputShape, layout);
  std::uint64_t allocations = 0;
  for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
    Invocation context;
    context.failAllocationAt(fail);
    Output output{};
    invoke(output, input, outputShape, context);
    const int expected = error ? error : fail && fail <= allocations ? 2 : 0;
    require(context.status() == expected, "reshape fault/allocation precedence changed");
    require(!std::memcmp(cells.data(), before.data(), cells.size() * sizeof(Cell)),
            "reshape mutated source");
    auto check = [&] {
#if OUTPUT_RANK > 0
      for (std::size_t axis = 0; axis < outputShape.size(); ++axis)
        require(output.sizes[axis] == outputShape[axis], "reshape output extent changed");
#endif
      for (std::size_t i = 0; i < count; ++i) {
        auto source = 1 + physical(input, i, inputShape);
        auto target = physical(output, i, outputShape);
        require(std::bit_cast<Raw>(output.aligned[target]) == std::bit_cast<Raw>(before[source]),
                "reshape changed exact bits or logical row-major order");
        ++lanes;
      }
    };
    std::optional<AllocationLease> lease;
    if (expected) {
      require(!output.allocated && !output.aligned, "failed reshape exposed partial tensor");
      if (error) {
        require(context.line() == 2 && context.column() == 3, "reshape validation lost source");
        require(!context.allocations(), "reshape validated after allocation");
      }
    } else {
      check();
      if (count) {
        lease = context.lease(output.allocated);
        require(bool(lease), "reshape result not retained");
        std::fill(cells.begin(), cells.end(), Cell{});
        check();
        std::memcpy(cells.data(), before.data(), cells.size() * sizeof(Cell));
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
    ++cases;
  }
}
#if INPUT_RANK == 2 && (OUTPUT_RANK == 2 || defined(TENSOR_TO_ARRAY)) && DIMENSION_KIND == 0 &&    \
    !defined(FIXED_RESHAPE)
static void cancellation(bool active) {
  std::vector<Cell> cells(active ? 1048576 : 1, Cell{7});
  Input input{};
  input.allocated = input.aligned = cells.data();
  input.sizes = active ? InputShape{1024, 1024} : InputShape{1, 1};
  input.strides = active ? InputShape{1024, 1} : InputShape{1, 1};
#ifdef TENSOR_TO_ARRAY
  OutputShape shape{active ? 1048576 : 1};
#else
  OutputShape shape = active ? OutputShape{2048, 512} : OutputShape{1, 1};
#endif
  Output output{};
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
  invoke(output, input, shape, context);
  done.store(true);
  if (active)
    sender.join();
  require(token.load() && context.poll() == -1 && !context.status() && !output.allocated &&
              !output.aligned,
          "cancelled reshape published output");
  if (active)
    require(polls.load() >= 128 && context.allocations(), "active reshape copy not entered");
  for (auto cell : cells)
    require(cell == Cell{7}, "cancelled reshape mutated input");
  context.settle();
  require(!context.live() && context.allocations() == context.releases(),
          "cancelled reshape leaked");
  ++cases;
}
#endif
template <std::size_t Rank> static void metadata() {
  CpuTensor<element, Rank> input{};
  OutputShape shape{};
  if constexpr (Rank > 0) {
    for (auto extent : {std::int64_t(-1), INT64_MAX}) {
      input.sizes.fill(1);
      input.strides.fill(1);
      input.sizes[0] = extent;
      Invocation context;
      Output output{};
      invoke(output, input, shape, context);
      require(context.status() == (extent < 0 ? 1 : 2) && context.line() == 1 &&
                  context.column() == 4 && !output.allocated && !output.aligned &&
                  !context.allocations(),
              "malformed source metadata reached reshape allocation");
      context.settle();
      require(!context.live(), "source metadata failure leaked");
      ++cases;
    }
  }
  Invocation context;
  std::atomic<bool> token{true};
  context.bindCancellation(&token);
  Output output{};
  invoke(output, input, shape, context);
  require(context.poll() == -1 && !context.status() && !output.allocated && !output.aligned &&
              !context.allocations(),
          "pre-entry cancellation published reshape output");
  context.settle();
  ++cases;
}
int main() {
  try {
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
      for (unsigned layout = 0; layout < 3; ++layout) {
#ifdef TENSOR_TO_ARRAY
#if INPUT_RANK == 0
        verify({}, {1}, layout);
#elif INPUT_RANK == 1
        verify({0}, {0}, layout);
        verify({1}, {1}, layout);
        verify({323}, {323}, layout);
#elif INPUT_RANK == 3
        verify({2, 3, 4}, {24}, layout);
        verify({2, 0, 4}, {0}, layout);
#elif INPUT_RANK == 8
        verify({1, 1, 1, 1, 1, 2, 3, 4}, {24}, layout);
        verify({1, 1, 1, 0, 1, 2, 3, 4}, {0}, layout);
#else
        for (auto shape : {InputShape{2, 3}, {3, 2}, {0, 3}, {2, 0}, {1, 1}, {17, 19}})
          verify(shape, {shape[0] * shape[1]}, layout);
#endif
#else
#if INPUT_RANK == 0
        verify({}, {1, 1}, layout);
        verify({}, {0, 1}, layout, 1);
#elif INPUT_RANK == 1
        verify({6}, {2, 3}, layout);
        verify({0}, {2, 0}, layout);
        verify({5}, {2, 3}, layout, 1);
#elif INPUT_RANK == 3
        verify({2, 3, 4}, {4, 6}, layout);
        verify({2, 0, 4}, {0, 4}, layout);
        verify({2, 3, 4}, {2, 13}, layout, 1);
#elif INPUT_RANK == 8
        verify({1, 1, 1, 1, 1, 2, 3, 4}, {4, 6}, layout);
        verify({1, 1, 1, 0, 1, 2, 3, 4}, {0, 4}, layout);
#else
#ifdef FIXED_RESHAPE
        verify({2, 2}, {2, 2}, layout);
        verify({2, 2}, {1, 4}, layout, 1);
#elif OUTPUT_RANK == 0
        verify({1, 1}, {}, layout);
        verify({2, 2}, {}, layout, 1);
        verify({0, 2}, {}, layout, 1);
#elif OUTPUT_RANK == 1
        for (auto shape : {InputShape{2, 3}, {3, 2}, {0, 3}, {2, 0}, {17, 19}})
          verify(shape, {shape[0] * shape[1]}, layout);
        verify({2, 3}, {5}, layout, 1);
        verify({2, 3}, {-1}, layout, 1);
#elif OUTPUT_RANK == 2
        for (auto shape : {InputShape{2, 3}, {3, 2}, {0, 3}, {2, 0}, {1, 1}, {17, 19}})
          verify(shape, {shape[1], shape[0]}, layout);
        verify({2, 3}, {2, 4}, layout, 1);
        verify({2, 3}, {-1, 0}, layout, 1);
        if constexpr (sizeof(D0) == 8) {
          verify({0, 3}, {INT64_MAX, 2}, layout, 2);
          verify({0, 3}, {0, INT64_MAX}, layout);
        }
        if constexpr (DIMENSION_KIND == 1)
          verify({0, 3}, {-1, 0}, layout, 1);
        if constexpr (DIMENSION_KIND == 2)
          verify({0, 3}, {0, UINT32_MAX}, layout);
#else
        verify({2, 3}, {1, 2, 3}, layout);
        verify({0, 3}, {1, 0, 3}, layout);
        verify({2, 3}, {1, 2, 4}, layout, 1);
        verify({0, 3}, {0, INT64_MAX, INT64_MAX}, layout, 2);
        verify({0, 3}, {INT64_MAX, INT64_MAX, 0}, layout);
#endif
#endif
#endif
      }
      state.checkFlags();
    }
#if INPUT_RANK == 2 && (OUTPUT_RANK == 2 || defined(TENSOR_TO_ARRAY)) && DIMENSION_KIND == 0 &&    \
    !defined(FIXED_RESHAPE)
    cancellation(false);
    cancellation(true);
#endif
    metadata<INPUT_RANK>();
#ifdef TENSOR_TO_ARRAY
    std::cout << "tensor to array "
#else
    std::cout << "tensor reshape "
#endif
              << describeNumeric(element)->name << " ranks " << INPUT_RANK << "->" << OUTPUT_RANK
              << " dimensions " << DIMENSION_KIND << ": " << cases << " cases, " << lanes
              << " exact lanes; strided row-major copies, shape/byte/stride faults, allocation "
                 "failures, cancellation and retained owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
