#include "../runtime/Runtime.h"
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>
#ifndef INTEGER_BITS
#define INTEGER_BITS 32
#endif
using Cell = std::conditional_t<INTEGER_BITS == 64, std::int64_t, std::int32_t>;
using Wide = std::conditional_t<INTEGER_BITS == 64, __int128_t, std::int64_t>;
constexpr auto minimum = std::numeric_limits<Cell>::min();
constexpr auto maximum = std::numeric_limits<Cell>::max();
extern "C" Cell _mlir_ciface_kalandra_test_entry(Cell, Cell, kalandra::runtime::Invocation *);
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view op = argv[1];
  std::uint64_t count = 0, faults = 0;
  auto check = [&](Cell a, Cell b) {
    const Wide x = a, y = b;
    bool bad = false;
    Wide expected = 0;
    if (op == "add")
      expected = x + y;
    else if (op == "sub")
      expected = x - y;
    else if (op == "mul")
      expected = x * y;
    else if (op == "neg")
      expected = -x;
    else if (op == "div" || op == "rem") {
      bad = y == 0;
      if (!bad)
        expected = op == "div" ? x / y : x % y;
    } else
      return false;
    bad |= expected < minimum || expected > maximum;
    kalandra::runtime::Invocation context;
    const auto result = _mlir_ciface_kalandra_test_entry(a, b, &context);
    ++count;
    if (bad)
      ++faults;
    if (context.live() || context.hasTaskError() || context.status() != (bad ? 4 : 0) ||
        (!bad && result != expected) || (bad && result != 0) || (bad && (context.line() != 2 || context.column() == 0))) {
      std::cerr << op << '(' << a << ',' << b << ") result=" << result << " expected="
                << (bad ? std::string("outside range")
                        : std::to_string(static_cast<std::int64_t>(expected)))
                << " status=" << context.status() << '\n';
      return false;
    }
    return true;
  };
  const Cell root = INTEGER_BITS == 64 ? Cell(3037000499LL) : Cell(46340);
  std::vector<Cell> edges{minimum,
                          minimum + 1,
                          minimum / 2,
                          -65536,
                          -root - 1,
                          -root,
                          -3,
                          -2,
                          -1,
                          0,
                          1,
                          2,
                          3,
                          root,
                          root + 1,
                          65536,
                          maximum / 2 + 1,
                          maximum - 1,
                          maximum};
  if constexpr (INTEGER_BITS == 64) {
    edges.push_back(Cell(9007199254740993LL));
    edges.push_back(Cell(-9007199254740993LL));
  }
  for (auto a : edges)
    for (auto b : edges)
      if (!check(a, b))
        return 1;
  std::uint32_t state = 0x12345678;
  auto next = [&] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  auto nextCell = [&]() -> Cell {
    if constexpr (INTEGER_BITS == 64) {
      const auto high = next();
      const auto low = next();
      return Cell(std::bit_cast<std::int64_t>((std::uint64_t(high) << 32) | low));
    } else
      return Cell(std::bit_cast<std::int32_t>(next()));
  };
  for (int i = 0; i < 10000; ++i) {
    const auto a = nextCell(), b = nextCell();
    if (!check(a, b))
      return 1;
  }
  if (!faults)
    return 1;
  std::cout << op << ": " << count << " runtime inputs, " << faults << " checked faults\n";
}
