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
extern "C" bool _mlir_ciface_kalandra_test_entry(Cell, Cell, Invocation *);
static bool reference(std::uint64_t a, std::uint64_t b) {
  llvm::APFloat::cmpResult order;
  if (isInteger(element)) {
    auto integer = [](std::uint64_t bits) -> __int128_t {
      llvm::APInt value(sizeof(Cell) * 8, bits);
      return isUnsigned(element) ? __int128_t(value.getZExtValue())
                                 : __int128_t(value.getSExtValue());
    };
    auto x = integer(a), y = integer(b);
    order = x < y   ? llvm::APFloat::cmpLessThan
            : x > y ? llvm::APFloat::cmpGreaterThan
                    : llvm::APFloat::cmpEqual;
  } else {
    const auto &format =
        sizeof(Cell) == 4 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
    llvm::APFloat x(format, llvm::APInt(sizeof(Cell) * 8, a)),
        y(format, llvm::APInt(sizeof(Cell) * 8, b));
    order = x.compare(y);
  }
  return OPERATION == 0   ? order == llvm::APFloat::cmpEqual
         : OPERATION == 1 ? order != llvm::APFloat::cmpEqual
         : OPERATION == 2 ? order == llvm::APFloat::cmpLessThan
         : OPERATION == 3 ? order == llvm::APFloat::cmpLessThan || order == llvm::APFloat::cmpEqual
         : OPERATION == 4
             ? order == llvm::APFloat::cmpGreaterThan
             : order == llvm::APFloat::cmpGreaterThan || order == llvm::APFloat::cmpEqual;
}
int main() {
  try {
    std::vector<Pair> inputs;
    std::vector<std::uint64_t> edges;
    const std::uint64_t mask = sizeof(Cell) == 4 ? UINT32_MAX : UINT64_MAX;
    if (isInteger(element)) {
      for (unsigned bit = 0; bit < sizeof(Cell) * 8; ++bit)
        for (std::uint64_t delta : {UINT64_C(0), UINT64_C(1), UINT64_MAX}) {
          auto value = ((UINT64_C(1) << bit) + delta) & mask;
          edges.push_back(value);
          edges.push_back((0 - value) & mask);
        }
    } else {
      const unsigned fraction = sizeof(Cell) == 4 ? 23 : 52,
                     exponents = sizeof(Cell) == 4 ? 256 : 2048;
      const auto mantissa = (UINT64_C(1) << fraction) - 1, sign = UINT64_C(1)
                                                                  << (sizeof(Cell) * 8 - 1);
      for (unsigned exponent : {0u, 1u, exponents / 2, exponents - 2, exponents - 1})
        for (auto tail : {UINT64_C(0), UINT64_C(1), mantissa / 2, mantissa / 2 + 1, mantissa}) {
          auto bits = (std::uint64_t(exponent) << fraction) | tail;
          edges.push_back(bits);
          edges.push_back(bits | sign);
        }
      for (unsigned exponent = 0; exponent < exponents; ++exponent)
        for (auto tail : {UINT64_C(0), UINT64_C(1), mantissa}) {
          auto bits = (std::uint64_t(exponent) << fraction) | tail;
          for (auto s : {UINT64_C(0), sign}) {
            inputs.push_back({bits | s, bits | s});
            inputs.push_back({bits | s, (bits + 1) | s});
            inputs.push_back({bits | s, (std::uint64_t(exponents - 1) << fraction) | 1});
          }
        }
    }
    for (auto a : edges)
      for (auto b : edges)
        inputs.push_back({a, b});
    std::uint64_t seed = 0x32e74a19bc04d65f;
    const auto random = [&] {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      return seed & mask;
    };
    for (unsigned i = 0; i < 20000; ++i) {
      auto a = random(), b = random();
      inputs.push_back({a, b});
      inputs.push_back({b, a});
    }
    std::vector<bool> expected;
    for (auto pair : inputs)
      expected.push_back(reference(pair.a, pair.b));
    std::uint64_t cases = 0;
    {
      FpState state;
      for (unsigned environment = 0; environment < 8; ++environment) {
        state.set(environment);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
          Invocation context;
          auto result =
              _mlir_ciface_kalandra_test_entry(std::bit_cast<Cell>(Raw(inputs[i].a)),
                                               std::bit_cast<Cell>(Raw(inputs[i].b)), &context);
          if (context.status() || result != expected[i])
            throw std::runtime_error("Boolean comparison differs from reference");
          context.settle();
          if (context.live() || context.allocations() != context.releases())
            throw std::runtime_error("Boolean comparison leaked owners");
          ++cases;
        }
        state.checkFlags();
      }
    }
    Invocation context;
    std::atomic<bool> token{true};
    context.bindCancellation(&token);
    if (_mlir_ciface_kalandra_test_entry(Cell{}, Cell{}, &context) || context.poll() != -1 ||
        context.status())
      throw std::runtime_error("cancelled comparison published true");
    context.settle();
    std::cout << "comparison " << describeNumeric(element)->name << " op " << OPERATION << ": "
              << cases
              << " exact Boolean results; eight FP environments and settled owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
