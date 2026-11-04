#include "../runtime/Tensor.h"
#include <array>
#include <atomic>
#include <cfenv>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
constexpr auto indexType = static_cast<NumericType>(INDEX_TYPE);
using Cell = NumericValue<element>;
using Position = NumericValue<indexType>;
using Raw = std::conditional_t<
    sizeof(Cell) == 8, std::uint64_t,
    std::conditional_t<sizeof(Cell) == 4, std::uint32_t,
                       std::conditional_t<sizeof(Cell) == 2, std::uint16_t, std::uint8_t>>>;
using Array = CpuTensor<element, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, Position, Cell, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Environment {
  std::fenv_t saved;
  Environment() { require(std::fegetenv(&saved) == 0, "FP environment unavailable"); }
  ~Environment() { std::fesetenv(&saved); }
};
int main() {
  try {
    Environment restore;
    constexpr std::uint64_t seeds[] = {0,
                                       1,
                                       UINT64_MAX,
                                       0x8000000000000000ULL,
                                       0x7ff0000000000001ULL,
                                       0x7ff8000000000042ULL,
                                       0x80000000,
                                       0x7f800001,
                                       0x7fc00042,
                                       0xff800001,
                                       0x7f800000,
                                       0x00800000};
    unsigned cases = 0;
    for (auto rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
      require(std::fesetround(rounding) == 0, "FP rounding mode unavailable");
      for (auto stride : {-2, -1, 0, 1, 2})
        for (auto position :
             {Position(0), Position(1), Position(3), Position(4), Position(-1),
              std::numeric_limits<Position>::max(), std::numeric_limits<Position>::min()})
          for (auto seed : seeds) {
            std::array<Cell, 12> cells{};
            for (unsigned i = 0; i < cells.size(); ++i) {
              Raw bits = static_cast<Raw>(i * 137 + 23);
              std::memcpy(&cells[i], &bits, sizeof(Cell));
            }
            auto expected = cells;
            Raw bits = static_cast<Raw>(seed);
            Cell value;
            std::memcpy(&value, &bits, sizeof(Cell));
            const std::int64_t offset = stride < 0 ? 8 : 2;
            Array input{cells.data(), cells.data(), offset, {4}, {stride}};
            const bool valid = static_cast<std::uint64_t>(position) < 4;
            if (valid)
              std::memcpy(&expected[offset + std::int64_t(position) * stride], &value,
                          sizeof(Cell));
            Invocation context;
            context.failAllocationAt(1); // Primitive stores must not allocate.
            std::feclearexcept(FE_ALL_EXCEPT);
            const auto result = _mlir_ciface_kalandra_test_entry(&input, position, value, &context);
            require(context.status() == (valid ? 0 : 5) && result == (valid ? 4 : 0),
                    "store changed bounds fault or result");
            require(std::memcmp(cells.data(), expected.data(), sizeof(cells)) == 0,
                    "store changed raw bits, addressing, or neighboring cells");
            require(!context.allocations() && !context.live(),
                    "primitive store allocated or retained storage");
            if (!valid)
              require(context.line() == 2 && context.column() == 3,
                      "store bounds fault location changed");
            require(!std::fetestexcept(FE_ALL_EXCEPT) && std::fegetround() == rounding,
                    "store changed FP environment or raised floating exceptions");
            context.settle();
            ++cases;
          }
    }
    for (auto mode : {0, 1, 2}) {
      std::array<Cell, 2> cells{Cell(1), Cell(2)}, original = cells;
      Array input{cells.data(), cells.data(), 0, {mode == 0 ? -1 : 2}, {1}};
      Invocation context;
      std::atomic<bool> cancelled{mode == 1};
      if (mode == 1)
        context.bindCancellation(&cancelled);
      if (mode == 2)
        context.fault(4);
      auto result = _mlir_ciface_kalandra_test_entry(&input, Position(0), Cell(7), &context);
      require(!result &&
                  context.status() == (mode == 0   ? 1
                                       : mode == 2 ? 4
                                                   : 0) &&
                  std::memcmp(cells.data(), original.data(), sizeof(cells)) == 0 &&
                  !context.allocations(),
              "metadata/cancellation/prior-fault exit wrote storage");
      if (mode == 1)
        require(context.poll() == -1, "store lost cancellation");
      context.settle();
    }
    std::cout << "Array stores: " << cases
              << " raw-bit/strided/index cases, four FP modes, no allocations, "
                 "bounds/cancellation/fault precedence passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
