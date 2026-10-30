#include "../runtime/Tensor.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <optional>
#include <type_traits>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Tensor = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::uint64_t cases = 0;
    for (std::size_t length : {0u, 1u, 2u, 3u, 17u, 4096u}) {
      std::vector<Cell> backing(2 * length + 8);
      for (std::size_t i = 0; i < backing.size(); ++i)
        backing[i] = std::bit_cast<Cell>(Raw(Raw(0xf17deacb) * Raw(i + 1)));
      const auto original = backing;
      Tensor input{};
      input.allocated = backing.data();
      input.aligned = backing.data() + 1;
      input.offset = 2;
      input.sizes = {std::int64_t(length)};
      input.strides = {2};
      std::uint64_t allocations = 0;
      for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
        Invocation context;
        context.failAllocationAt(fail);
        Tensor output{};
        _mlir_ciface_kalandra_test_entry(&output, &input, &context);
        ++cases;
        const bool shapeFailure = length != 2;
        const bool allocationFailure = !shapeFailure && fail && fail <= allocations;
        require(context.status() == (shapeFailure        ? 1
                                     : allocationFailure ? 2
                                                         : 0),
                "shape/allocation status or precedence changed");
        if (shapeFailure)
          require(context.line() == 2 && context.column() == 10,
                  "array shape failure lost original source location");
        require(!std::memcmp(backing.data(), original.data(), backing.size() * sizeof(Cell)),
                "constructor mutated borrowed input");
        auto verify = [&] {
          require(output.sizes[0] == 2, "fixed tensor extent lost");
          for (std::size_t i = 0; i < 2; ++i)
            require(std::bit_cast<Raw>(
                        output.aligned[output.offset + std::int64_t(i) * output.strides[0]]) ==
                        std::bit_cast<Raw>(original[3 + 2 * i]),
                    "constructor changed element bits/order or aliased borrowed input");
        };
        std::optional<AllocationLease> retained;
        if (shapeFailure || allocationFailure)
          require(!output.allocated && !output.aligned,
                  "failed constructor published partial tensor");
        else {
          verify();
          require(output.allocated != input.allocated,
                  "borrowed array storage escaped as owned tensor");
          retained = context.lease(output.allocated);
          require(bool(retained), "constructed tensor backing not retained");
          std::fill(backing.begin(), backing.end(), Cell{});
          verify();
          std::memcpy(backing.data(), original.data(), backing.size() * sizeof(Cell));
        }
        if (!fail) {
          allocations = context.allocations();
          require(allocations < 16, "unbounded constructor allocation baseline");
          if (shapeFailure)
            require(!allocations, "shape validation happened after allocation");
        }
        context.settle();
        require(!context.live() && context.allocations() == context.releases(),
                "constructor leaked allocation owners");
        if (!shapeFailure && !allocationFailure)
          verify();
      }
    }
    std::cout << "array tensor shape " << describeNumeric(element)->name << ": " << cases
              << " cases; exact copies, source locations, shape-before-allocation, borrowed input "
                 "independence and retained owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
