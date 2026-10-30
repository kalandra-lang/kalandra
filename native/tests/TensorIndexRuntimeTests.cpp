#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Tensor = CpuTensor<element, TENSOR_RANK>;
#ifndef INDEX_KIND
#define INDEX_KIND 0
#endif
#if INDEX_KIND == 1
using D0 = std::uint64_t;
using D1 = std::uint64_t;
#elif INDEX_KIND == 2
using D0 = std::int32_t;
using D1 = std::uint32_t;
#elif INDEX_KIND == 3
using D0 = std::uint32_t;
using D1 = std::int32_t;
#else
using D0 = std::int64_t;
using D1 = std::int64_t;
#endif
extern "C" Cell _mlir_ciface_kalandra_test_entry(Tensor *
#if TENSOR_RANK > 0
                                                 ,
                                                 D0
#endif
#if TENSOR_RANK > 1
                                                 ,
                                                 D1
#endif
#if TENSOR_RANK > 2
                                                 ,
                                                 D0
#endif
#if TENSOR_RANK > 3
                                                 ,
                                                 D0, D0, D0, D0, D0
#endif
                                                 ,
                                                 Invocation *);
using Indices = std::array<std::uint64_t, TENSOR_RANK>;
static Cell invoke(Tensor &input, const Indices &indices, Invocation &context) {
  (void)indices;
  return _mlir_ciface_kalandra_test_entry(&input
#if TENSOR_RANK > 0
                                          ,
                                          D0(indices[0])
#endif
#if TENSOR_RANK > 1
                                              ,
                                          D1(indices[1])
#endif
#if TENSOR_RANK > 2
                                              ,
                                          D0(indices[2])
#endif
#if TENSOR_RANK > 3
                                              ,
                                          D0(indices[3]), D0(indices[4]), D0(indices[5]),
                                          D0(indices[6]), D0(indices[7])
#endif
                                                              ,
                                          &context);
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::uint64_t cases = 0, faults = 0;
template <std::size_t Rank>
static void verify(std::array<std::int64_t, Rank> shape, unsigned layout, unsigned pattern) {
  std::size_t count = 1;
  for (auto d : shape)
    count *= std::size_t(d);
  std::vector<Cell> cells(2 * count + 32);
  const Raw edges[] = {0,
                       Raw(Raw(1) << (8 * sizeof(Cell) - 1)),
                       1,
                       Raw(-1),
                       sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                       sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL),
                       sizeof(Cell) == 4 ? Raw(0x7fc12345) : Raw(0x7ff812345678abcdULL),
                       sizeof(Cell) == 4 ? Raw(0x007fffff) : Raw(0x000fffffffffffffULL)};
  for (std::size_t i = 0; i < cells.size(); ++i) {
    Raw bits = Raw(0xf17deacb1297ff83ULL) * Raw(i + pattern + 1);
    if (i < 16)
      bits = edges[(i + pattern) % 8];
    cells[i] = std::bit_cast<Cell>(bits);
  }
  const auto original = cells;
  CpuTensor<element, Rank> input{};
  input.allocated = cells.data();
  input.aligned = cells.data() + 1;
  input.offset = 2;
  if constexpr (Rank > 0) {
    input.sizes = shape;
    std::int64_t stride = 2;
    for (std::size_t i = 0; i < Rank; ++i) {
      auto axis = layout == 1 ? i : Rank - i - 1;
      input.strides[axis] = layout == 2 ? -stride : stride;
      if (layout == 2 && shape[axis] > 0)
        input.offset += (shape[axis] - 1) * stride;
      stride *= shape[axis] ? shape[axis] : 1;
    }
  }
  std::vector<Indices> points;
  for (std::size_t flat = 0; flat < count; ++flat) {
    auto remainder = flat;
    Indices point{};
    for (std::size_t axis = Rank; axis-- > 0;) {
      point[axis] = remainder % shape[axis];
      remainder /= shape[axis];
    }
    points.push_back(point);
  }
  if constexpr (Rank > 0) {
    for (std::size_t axis = 0; axis < Rank; ++axis)
      for (auto position : {std::uint64_t(shape[axis]), UINT64_MAX, std::uint64_t(INT64_MAX),
                            std::uint64_t(1) << 63, std::uint64_t(1) << 32}) {
        Indices point{};
        point[axis] = position;
        points.push_back(point);
      }
  }
  for (auto point : points) {
    // Reflect each axis' concrete signedness and width, including mixed indices.
    for (std::size_t axis = 0; axis < Rank; ++axis)
      point[axis] = axis == 1 ? std::uint64_t(D1(point[axis])) : std::uint64_t(D0(point[axis]));
    bool valid = true;
    auto physical = 1 + input.offset;
    if constexpr (Rank > 0)
      for (std::size_t axis = 0; axis < Rank; ++axis) {
        valid &= point[axis] < std::uint64_t(shape[axis]);
        if (valid)
          physical += std::int64_t(point[axis]) * input.strides[axis];
      }
    Invocation context;
    auto output = invoke(input, point, context);
    require(context.status() == (valid ? 0 : 5), "wrong per-axis bounds result");
    if (valid)
      require(std::bit_cast<Raw>(output) == std::bit_cast<Raw>(original[physical]),
              "tensor read changed exact bits or descriptor addressing");
    else {
      require(context.line() == 2 && context.column() == 3 && std::bit_cast<Raw>(output) == 0,
              "tensor bounds source/result changed");
      ++faults;
    }
    require(!context.allocations() &&
                !std::memcmp(cells.data(), original.data(), cells.size() * sizeof(Cell)),
            "tensor read allocated or mutated borrowed input");
    context.settle();
    require(!context.live(), "tensor read leaked");
    ++cases;
  }
}
template <std::size_t Rank> static void metadata() {
  CpuTensor<element, Rank> input{};
  if constexpr (Rank > 0) {
    input.sizes.fill(1);
    input.strides.fill(1);
    input.sizes[0] = -1;
    Invocation negative;
    auto output = invoke(input, {}, negative);
    require(negative.status() == 1 && std::bit_cast<Raw>(output) == 0 && !negative.allocations(),
            "negative metadata reached read");
    negative.settle();
    input.sizes[0] = INT64_MAX;
    Invocation overflow;
    output = invoke(input, {}, overflow);
    require(overflow.status() == 2 && std::bit_cast<Raw>(output) == 0 && !overflow.allocations(),
            "overflow metadata reached read");
    overflow.settle();
  }
  Invocation context;
  std::atomic<bool> cancelled{true};
  context.bindCancellation(&cancelled);
  auto output = invoke(input, {}, context);
  require(context.poll() == -1 && !context.status() && std::bit_cast<Raw>(output) == 0,
          "cancelled tensor read reached memory");
  context.settle();
}
int main() {
  try {
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
      for (unsigned layout = 0; layout < 3; ++layout) {
#if TENSOR_RANK == 0
        for (unsigned pattern = 0; pattern < 8; ++pattern)
          verify<0>({}, layout, pattern);
#elif TENSOR_RANK == 1
        for (std::int64_t length : {0, 1, 2, 17, 257})
          verify<1>({length}, layout, 0);
#elif TENSOR_RANK == 2
        for (auto shape : {std::array<std::int64_t, 2>{2, 3}, {0, 3}, {2, 0}, {1, 1}, {17, 13}})
          verify<2>(shape, layout, 0);
#elif TENSOR_RANK == 3
        for (auto shape :
             {std::array<std::int64_t, 3>{2, 3, 4}, {0, 3, 4}, {2, 0, 4}, {2, 3, 0}, {1, 1, 1}})
          verify<3>(shape, layout, 0);
#else
        for (auto shape : {std::array<std::int64_t, 8>{1, 1, 1, 1, 1, 1, 1, 1},
                           {2, 2, 2, 2, 2, 2, 2, 3},
                           {2, 2, 2, 0, 2, 2, 2, 3}})
          verify<8>(shape, layout, 0);
#endif
      }
      state.checkFlags();
    }
    metadata<TENSOR_RANK>();
    std::cout << "tensor index " << describeNumeric(element)->name << " rank " << TENSOR_RANK
              << " indices " << INDEX_KIND << ": " << cases << " cases, " << faults
              << " bounds faults; exact bits, strided borrowed input, eight FP states, "
                 "metadata/cancellation and settled owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
