#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<
    sizeof(Cell) == 1, std::uint8_t,
    std::conditional_t<sizeof(Cell) == 2, std::uint16_t,
                       std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>>>;
using Tensor = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Tensor *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::vector<Cell> cells(std::size_t n, unsigned salt) {
  std::vector<Cell> result(2 * n + 8);
  const Raw edges[] = {Raw(0),
                       Raw(1),
                       Raw(-1),
                       Raw(UINT64_C(1) << (sizeof(Cell) * 8 - 1)),
                       sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                       sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL),
                       sizeof(Cell) == 4 ? Raw(0x7fc12345) : Raw(0x7ff812345678abcdULL)};
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] =
        std::bit_cast<Cell>(Raw(i < 7 ? edges[i] : Raw(0xf17deacb1297ff83ULL) * Raw(i + salt)));
  return result;
}
static Tensor view(std::vector<Cell> &data, std::int64_t size, std::int64_t stride) {
  Tensor result{};
  result.allocated = data.data();
  result.aligned = data.data() + 1;
  result.offset = stride < 0 ? size + 1 : 1;
  result.sizes = {size};
  result.strides = {stride};
  return result;
}
int main() {
  try {
    unsigned cases = 0, lanes = 0;
    for (std::int64_t a : {0, 1, 7, 33})
      for (std::int64_t b : {0, 1, 7, 33})
        for (std::int64_t sa : {1, 2, -1})
          for (std::int64_t sb : {1, 2, -1}) {
            auto ac = cells(a, 1), bc = cells(b, 13);
            auto originalA = ac, originalB = bc;
            auto left = view(ac, a, sa), right = view(bc, b, sb);
            std::vector<Raw> expected;
            for (auto *input : {&left, &right})
              for (std::int64_t i = 0; i < input->sizes[0]; ++i)
                expected.push_back(
                    std::bit_cast<Raw>(input->aligned[input->offset + i * input->strides[0]]));
            Invocation context;
            Tensor output{};
            _mlir_ciface_kalandra_test_entry(&output, &left, &right, &context);
            require(!context.status() && output.sizes[0] == a + b, "concat length/status mismatch");
            auto check = [&] {
              for (std::size_t i = 0; i < expected.size(); ++i)
                require(std::bit_cast<Raw>(output.aligned[output.offset + i * output.strides[0]]) ==
                            expected[i],
                        "concat changed order, stride addressing or raw bits");
            };
            check();
            lanes += expected.size();
            require(!std::memcmp(ac.data(), originalA.data(), ac.size() * sizeof(Cell)) &&
                        !std::memcmp(bc.data(), originalB.data(), bc.size() * sizeof(Cell)),
                    "concat mutated input");
            auto lease = expected.empty() ? std::optional<AllocationLease>{}
                                          : context.lease(output.allocated);
            require(expected.empty() || bool(lease), "concat output was borrowed");
            std::fill(ac.begin(), ac.end(), Cell{});
            std::fill(bc.begin(), bc.end(), Cell{});
            context.settle();
            require(!context.live() && context.allocations() == context.releases(),
                    "concat leaked owners");
            check();
            ++cases;
          }
    auto storage = cells(7, 1);
    auto input = view(storage, 7, 1);
    Invocation alias;
    Tensor output{};
    _mlir_ciface_kalandra_test_entry(&output, &input, &input, &alias);
    require(!alias.status() && output.sizes[0] == 14, "aliased concat failed");
    for (unsigned i = 0; i < 14; ++i)
      require(std::bit_cast<Raw>(output.aligned[output.offset + i * output.strides[0]]) ==
                  std::bit_cast<Raw>(input.aligned[input.offset + i % 7]),
              "aliased concat lost order");
    alias.settle();
    require(!alias.live(), "alias output leaked");
    for (unsigned mode = 0; mode < 2; ++mode) {
      Invocation context;
      std::atomic<bool> cancelled{true};
      if (mode == 0)
        context.failAllocationAt(1);
      else
        context.bindCancellation(&cancelled);
      output = {};
      _mlir_ciface_kalandra_test_entry(&output, &input, &input, &context);
      require(mode == 0 ? context.status() == 2 : context.poll() < 0,
              "concat failure gate missing");
      require(!output.allocated && !output.sizes[0], "failed concat exposed result");
      context.settle();
      require(!context.live(), "failed concat leaked");
    }
    for (unsigned side = 0; side < 2; ++side)
      for (auto size : {std::int64_t(-1), INT64_MAX}) {
        auto bad = input;
        bad.sizes = {size};
        Invocation context;
        output = {};
        _mlir_ciface_kalandra_test_entry(&output, side ? &input : &bad, side ? &bad : &input,
                                         &context);
        require(context.status() == (size < 0 ? 1 : 2) && !context.allocations() &&
                    !output.allocated,
                "bad concat metadata reached allocation/read");
        context.settle();
        require(!context.live(), "bad concat metadata leaked");
      }
    // Individually representable inputs can still exceed the result byte budget.
    auto huge = input;
    huge.sizes = {INT64_MAX / (2 * sizeof(Cell)) + 1};
    Invocation overflow;
    output = {};
    _mlir_ciface_kalandra_test_entry(&output, &huge, &huge, &overflow);
    require(overflow.status() == 2 && !overflow.allocations() && !output.allocated,
            "combined byte count overflow reached allocation or copy");
    overflow.settle();
    require(!overflow.live(), "overflow leaked");
    std::cout << "Array concat " << describeNumeric(element)->name << ": " << cases << " pairs, "
              << lanes
              << " exact lanes; offsets/strides, aliasing, retained ownership and failure cleanup "
                 "passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
