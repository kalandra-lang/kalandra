#include "../runtime/Runtime.h"
#include <array>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>
#ifndef INTEGER_BITS
#define INTEGER_BITS 32
#endif
using Cell = std::conditional_t<INTEGER_BITS == 64, std::uint64_t, std::uint32_t>;
using Wide = std::conditional_t<INTEGER_BITS == 64, __uint128_t, std::uint64_t>;
constexpr auto maximum = std::numeric_limits<Cell>::max();
extern "C" Cell _mlir_ciface_kalandra_test_entry(Cell, Cell, kalandra::runtime::Invocation *);
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const std::string_view op = argv[1];
  std::uint64_t cases = 0, faults = 0;
  auto verify = [&](Cell a, Cell b) {
    const Wide x = a, y = b;
    Wide expected = 0;
    bool bad = false;
    if (op == "add")
      expected = x + y;
    else if (op == "sub") {
      bad = x < y;
      if (!bad)
        expected = x - y;
    } else if (op == "mul")
      expected = x * y;
    else if (op == "div" || op == "rem") {
      bad = y == 0;
      if (!bad)
        expected = op == "div" ? x / y : x % y;
    } else if (op == "neg")
      bad = x != 0;
    else if (op == "pos")
      expected = x;
    else
      return false;
    bad |= expected > maximum;
    kalandra::runtime::Invocation context;
    auto result = _mlir_ciface_kalandra_test_entry(a, b, &context);
    ++cases;
    faults += bad;
    if (context.live() || context.hasTaskError() || context.status() != (bad ? 4 : 0) ||
        (!bad && result != expected) ||
        (bad && (result != 0 || context.line() != 2 || context.column() != 3))) {
      std::cerr << op << '(' << a << ',' << b << ") result=" << result << " expected="
                << (bad ? std::string("fault")
                        : std::to_string(static_cast<std::uint64_t>(expected)))
                << " status=" << context.status() << '\n';
      return false;
    }
    return true;
  };
  std::vector<Cell> edges{0,
                          1,
                          2,
                          3,
                          255,
                          256,
                          65535,
                          65536,
                          65537,
                          16777217,
                          2147483647,
                          2147483648,
                          2147483649,
                          UINT32_MAX - 2,
                          UINT32_MAX - 1,
                          UINT32_MAX};
  if constexpr (INTEGER_BITS == 64) {
    for (auto value : {4294967296ULL, 4294967297ULL, 9007199254740993ULL, 9223372036854775807ULL,
                       9223372036854775808ULL, 9223372036854775809ULL, UINT64_MAX - 2,
                       UINT64_MAX - 1, UINT64_MAX})
      edges.push_back(Cell(value));
  }
  for (auto a : edges)
    for (auto b : edges)
      if (!verify(a, b))
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
      return Cell((std::uint64_t(high) << 32) | low);
    } else
      return next();
  };
  for (unsigned i = 0; i < 10000; ++i) {
    auto a = nextCell(), b = nextCell();
    if (!verify(a, b))
      return 1;
  }
  std::cout << "uint" << INTEGER_BITS << " " << op << ": " << cases << " runtime inputs, " << faults
            << " checked faults passed\n";
}
