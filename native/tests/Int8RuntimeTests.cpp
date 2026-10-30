#include "../runtime/Runtime.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>
#ifndef INTEGER_BITS
#define INTEGER_BITS 8
#endif
static_assert(INTEGER_BITS == 8 || INTEGER_BITS == 16);
using Signed = std::conditional_t<INTEGER_BITS == 8, std::int8_t, std::int16_t>;
using Unsigned = std::conditional_t<INTEGER_BITS == 8, std::uint8_t, std::uint16_t>;
using Cell = std::conditional_t<UNSIGNED_VALUE, Unsigned, Signed>;
#ifdef COMPARISON
extern "C" bool _mlir_ciface_kalandra_test_entry(Cell, Cell, kalandra::runtime::Invocation *);
#else
extern "C" Cell _mlir_ciface_kalandra_test_entry(Cell, Cell, kalandra::runtime::Invocation *);
#endif
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::string_view op = argv[1];
  std::uint64_t cases = 0, faults = 0;
  auto verify = [&](std::int64_t x, std::int64_t y) {
    std::int64_t expected = 0;
    bool invalid = false;
    if (op == "add")
      expected = x + y;
    else if (op == "sub")
      expected = x - y;
    else if (op == "mul")
      expected = x * y;
    else if (op == "neg")
      expected = -x;
    else if (op == "div" || op == "rem") {
      invalid = y == 0;
      if (!invalid)
        expected = op == "div" ? x / y : x % y;
    } else if (op == "eq")
      expected = x == y;
    else if (op == "ne")
      expected = x != y;
    else if (op == "lt")
      expected = x < y;
    else if (op == "le")
      expected = x <= y;
    else if (op == "gt")
      expected = x > y;
    else if (op == "ge")
      expected = x >= y;
    else
      return false;
#ifndef COMPARISON
      invalid |= expected < std::numeric_limits<Cell>::min() ||
                 expected > std::numeric_limits<Cell>::max();
#endif
    kalandra::runtime::Invocation ctx;
    auto result = _mlir_ciface_kalandra_test_entry(Cell(x), Cell(y), &ctx);
    if (ctx.status() != (invalid ? 4 : 0) || int(result) != (invalid ? 0 : expected) ||
        (invalid && (ctx.line() != 2 || ctx.column() != 3)) || ctx.allocations()) {
      std::cerr << op << "(" << x << "," << y << "): " << int(result) << " expected " << expected
                << " status " << ctx.status() << '\n';
      return false;
    }
    ctx.settle();
    if (ctx.live())
      return false;
    ++cases;
    faults += invalid;
    return true;
  };
  constexpr std::int64_t low = std::numeric_limits<Cell>::min();
  constexpr std::int64_t high = std::numeric_limits<Cell>::max();
  if constexpr (INTEGER_BITS == 8) {
    for (auto x = low; x <= high; ++x)
      for (auto y = op == "neg" ? 0 : low; y <= (op == "neg" ? 0 : high); ++y)
        if (!verify(x, y))
          return 1;
  } else {
    std::vector<std::int64_t> edges{low, low + 1, low + 2, high, high - 1, high - 2, 0, 1, 2};
    for (unsigned bit = 0; bit < 16; ++bit)
      for (auto delta : {-1, 0, 1})
        for (auto sign : {-1, 1}) {
          auto value = sign * (INT64_C(1) << bit) + delta;
          if (value >= low && value <= high)
            edges.push_back(value);
        }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (auto x = low; x <= high; ++x) {
      if (op == "neg") {
        if (!verify(x, 0))
          return 1;
      } else
        for (auto y : edges)
          if (!verify(x, y) || !verify(y, x))
            return 1;
    }
    if (op != "neg") {
      std::uint64_t seed = UINT64_C(0xc26a79d1b083f54e);
      auto next = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return low + static_cast<std::int64_t>(seed & 65535);
      };
      for (unsigned i = 0; i < 65536; ++i) {
        auto x = next(), y = next();
        if (!verify(x, y))
          return 1;
      }
    }
  }
  std::cout << (UNSIGNED_VALUE ? "uint" : "int") << INTEGER_BITS << " " << op << ": " << cases
            << (INTEGER_BITS == 8 ? " exhaustive pairs, " : " all-value/boundary/random cases, ")
            << faults << " checked faults passed\n";
}
