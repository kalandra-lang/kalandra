#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <optional>
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
using Array = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Array *, Array *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    unsigned cases = 0;
    for (std::int64_t count : {0, 1, 17, 4097})
      for (std::int64_t stride : {1, 2, -1}) {
        std::vector<Cell> cells(2 * count + 8);
        for (std::size_t i = 0; i < cells.size(); ++i) {
          const Raw edges[] = {Raw(0),
                               Raw(1),
                               Raw(-1),
                               Raw(UINT64_C(1) << (sizeof(Cell) * 8 - 1)),
                               sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL),
                               sizeof(Cell) == 4 ? Raw(0x7f800001) : Raw(0x7ff0000000000001ULL)};
          cells[i] = std::bit_cast<Cell>(i < 6 ? edges[i] : Raw(0x9e3779b97f4a7c15ULL * i));
        }
        const auto original = cells;
        Array input{};
        input.allocated = cells.data();
        input.aligned = cells.data() + 1;
        input.offset = stride < 0 ? count + 1 : 1;
        input.sizes = {count};
        input.strides = {stride};
        std::uint64_t allocations = 0;
        for (std::uint64_t fail = 0; fail <= allocations + 1; ++fail) {
          cells = original;
          Invocation context;
          context.failAllocationAt(fail);
          Array output{};
          _mlir_ciface_kalandra_test_entry(&output, &input, &context);
          const bool failed = fail && fail <= allocations;
          require(context.status() == (failed ? 2 : 0), "copy allocation status mismatch");
          auto verify = [&] {
            require(output.sizes[0] == count, "copy changed length");
            for (std::int64_t i = 0; i < count; ++i)
              require(std::bit_cast<Raw>(output.aligned[output.offset + i * output.strides[0]]) ==
                          std::bit_cast<Raw>(original[1 + input.offset + i * stride]),
                      "copy changed raw bits/order or retained source alias");
          };
          std::optional<AllocationLease> retained;
          if (failed)
            require(!output.allocated && !output.aligned, "failed copy exposed partial result");
          else {
            verify();
            if (count) {
              require(output.allocated != input.allocated, "copy reused source backing");
              retained = context.lease(output.allocated);
              require(bool(retained), "copy output has no owner");
            }
          }
          require(!std::memcmp(cells.data(), original.data(), cells.size() * sizeof(Cell)),
                  "copy mutated source");
          if (!fail) {
            allocations = context.allocations();
            require(allocations < 16, "copy allocated per element");
          }
          std::fill(cells.begin(), cells.end(), Cell{});
          context.settle();
          require(!context.live() && context.allocations() == context.releases(), "copy leaked");
          if (!failed)
            verify();
          ++cases;
        }
      }
    std::cout << "Independent copy " << describeNumeric(element)->name << ": " << cases
              << " raw-bit/strided cases; source mutation, retained owners and allocation faults "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
