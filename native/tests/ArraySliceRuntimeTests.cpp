#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
constexpr auto bound = static_cast<NumericType>(BOUND_TYPE);
using Cell = NumericValue<element>;
using Position = NumericValue<bound>;
using Raw = std::conditional_t<
    sizeof(Cell) == 1, std::uint8_t,
    std::conditional_t<sizeof(Cell) == 2, std::uint16_t,
                       std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>>>;
using Tensor = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Position, Position,
                                                 Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    unsigned cases = 0, faults = 0;
    for (std::int64_t length : {0, 1, 7, 33})
      for (std::int64_t stride : {1, 2, -1}) {
        std::vector<Cell> cells(2 * length + 8);
        for (std::size_t i = 0; i < cells.size(); ++i) {
          const Raw edges[] = {Raw(0),
                               Raw(1),
                               Raw(-1),
                               Raw(UINT64_C(1) << (sizeof(Cell) * 8 - 1)),
                               sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                               sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL),
                               sizeof(Cell) == 4 ? Raw(0x7fc12345) : Raw(0x7ff812345678abcdULL)};
          cells[i] =
              std::bit_cast<Cell>(Raw(i < 7 ? edges[i] : Raw(0xf17deacb1297ff83ULL) * Raw(i + 1)));
        }
        auto original = cells;
        Tensor input{};
        input.allocated = cells.data();
        input.aligned = cells.data() + 1;
        input.offset = stride < 0 ? length + 1 : 1;
        input.sizes = {length};
        input.strides = {stride};
        std::vector<Position> bounds = {Position(0),
                                        Position(1),
                                        Position(length / 2),
                                        Position(length),
                                        Position(length + 1),
                                        Position(-1),
                                        std::numeric_limits<Position>::min(),
                                        std::numeric_limits<Position>::max()};
        for (auto start : bounds)
          for (auto end : bounds) {
            Invocation context;
            Tensor output{};
            _mlir_ciface_kalandra_test_entry(&output, &input, start, end, &context);
            bool valid = std::uint64_t(start) <= std::uint64_t(end) &&
                         std::uint64_t(end) <= std::uint64_t(length);
            require(context.status() == (valid ? 0 : 5), "slice Bounds status mismatch");
            std::optional<AllocationLease> kept;
            if (valid) {
              require(output.sizes[0] == std::int64_t(end - start), "slice size mismatch");
              for (std::int64_t i = 0; i < output.sizes[0]; ++i) {
                auto expected = original[1 + input.offset + (std::int64_t(start) + i) * stride];
                require(std::bit_cast<Raw>(output.aligned[output.offset + i * output.strides[0]]) ==
                            std::bit_cast<Raw>(expected),
                        "slice changed exact bits or stride addressing");
              }
              if (output.sizes[0]) {
                kept = context.lease(output.allocated);
                require(bool(kept), "slice output not owned");
              }
            } else {
              ++faults;
              require(context.line() == 2 && context.column() == 3 && !context.allocations(),
                      "bad range allocated or lost source location");
              require(!output.allocated && !output.aligned && !output.sizes[0],
                      "fault published partial output");
            }
            require(!std::memcmp(cells.data(), original.data(), cells.size() * sizeof(Cell)),
                    "slice mutated input");
            context.settle();
            require(!context.live() && context.allocations() == context.releases(),
                    "slice leaked allocation");
            if (kept)
              require(
                  std::bit_cast<Raw>(output.aligned[output.offset]) ==
                      std::bit_cast<Raw>(original[1 + input.offset + std::int64_t(start) * stride]),
                  "retained slice lost after settlement");
            ++cases;
          }
      }
    std::vector<Cell> cells(32);
    Tensor input{};
    input.allocated = input.aligned = cells.data();
    input.sizes = {32};
    input.strides = {1};
    for (unsigned mode = 0; mode < 2; ++mode) {
      Invocation context;
      std::atomic<bool> cancellation{true};
      if (mode == 0)
        context.failAllocationAt(1);
      if (mode == 1)
        context.bindCancellation(&cancellation);
      Tensor output{};
      _mlir_ciface_kalandra_test_entry(&output, &input, Position(1), Position(3), &context);
      require(mode == 0 ? context.status() == 2 : context.poll() < 0,
              "allocation/cancellation gate missing");
      require(!output.allocated && !output.sizes[0], "failed slice published output");
      context.settle();
      require(!context.live(), "failed slice leaked");
    }
    for (auto size : {std::int64_t(-1), INT64_MAX}) {
      input.sizes = {size};
      Invocation context;
      Tensor output{};
      _mlir_ciface_kalandra_test_entry(&output, &input, Position(0), Position(0), &context);
      // INT64_MAX one-byte cells fit the descriptor byte budget. Selecting
      // an empty range may construct an owned empty result without reading input.
      const bool representableEmpty = size == INT64_MAX && sizeof(Cell) == 1;
      require(context.status() == (size < 0             ? 1
                                   : representableEmpty ? 0
                                                        : 2) &&
                  !output.sizes[0],
              "array size guard or representable empty slice changed");
      if (!representableEmpty)
        require(!context.allocations() && !output.allocated,
                "invalid array size reached slicing or allocation");
      else
        require(bool(context.lease(output.allocated)), "empty byte slice is not owned");
      context.settle();
      require(!context.live(), "invalid metadata leaked");
    }
    std::cout
        << "Array slice " << describeNumeric(element)->name << "/" << describeNumeric(bound)->name
        << ": " << cases << " ranges, " << faults
        << " bounds faults; exact bits, strides, retained ownership and failure cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
