#include "../metal-proof/Float64Oracle.h"
#include "../runtime/NumericStorage.h"
#include "../runtime/Runtime.h"
#include <bit>
#include <iostream>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>;
extern "C" Cell _mlir_ciface_kalandra_test_entry(bool, Cell, Cell, Invocation *);
int main() {
  try {
    std::uint64_t cases = 0, seed = 0x765acfed08964132;
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
      for (unsigned i = 0; i < 8192; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        auto a = Raw(seed);
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        auto b = Raw(seed);
        const std::uint64_t edges32[] = {0,          0x80000000, 1,          0x7f800000,
                                         0xff800000, 0x7fc01234, 0x7f800001, 0xffffffff};
        const std::uint64_t edges64[] = {0,
                                         UINT64_C(0x8000000000000000),
                                         1,
                                         UINT64_C(0x7ff0000000000000),
                                         UINT64_C(0xfff0000000000000),
                                         UINT64_C(0x7ff8123456789abc),
                                         UINT64_C(0x7ff0000000000001),
                                         UINT64_MAX};
        if (i < 8) {
          auto edges = sizeof(Cell) == 4 ? edges32 : edges64;
          a = Raw(edges[i]);
          b = Raw(edges[7 - i]);
        }
        for (bool flag : {false, true}) {
          Invocation context;
          auto result = _mlir_ciface_kalandra_test_entry(flag, std::bit_cast<Cell>(a),
                                                         std::bit_cast<Cell>(b), &context);
          if (std::bit_cast<Raw>(result) != (flag ? a : b) || context.status())
            throw std::runtime_error("conditional changed selected scalar bits");
          context.settle();
          if (context.live() || context.allocations() != context.releases())
            throw std::runtime_error("conditional leaked owners");
          ++cases;
        }
      }
      state.checkFlags();
    }
    std::cout << "conditional " << describeNumeric(element)->name << ": " << cases
              << " exact selected values; eight FP environments and settled owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
