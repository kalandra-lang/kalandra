#include "../runtime/Tensor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<
    sizeof(Cell) == 8, std::uint64_t,
    std::conditional_t<sizeof(Cell) == 4, std::uint32_t,
                       std::conditional_t<sizeof(Cell) == 2, std::uint16_t, std::uint8_t>>>;
using Array = CpuTensor<element, 1>;
#ifdef SORT_COPY
extern "C" void _mlir_ciface_kalandra_test_entry(Array *, Array *, Invocation *);
#else
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, Invocation *);
#endif
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static bool less(Raw a, Raw b) {
  if constexpr (element == NumericType::Float32 || element == NumericType::Float64) {
    constexpr Raw sign = Raw(1) << (sizeof(Raw) * 8 - 1);
    const auto key = [&](Raw bits) -> Raw { return bits & sign ? ~bits : bits ^ sign; };
    return key(a) < key(b);
  } else if constexpr (std::is_signed_v<Cell>) {
    return std::bit_cast<Cell>(a) < std::bit_cast<Cell>(b);
  } else
    return a < b;
}
int main() {
  std::fenv_t saved;
  std::fegetenv(&saved);
  try {
    std::mt19937_64 random(0x53a71);
    const std::array<std::uint64_t, 20> edges{0,
                                              1,
                                              UINT64_MAX,
                                              0x8000000000000000ULL,
                                              0x7ff0000000000000ULL,
                                              0xfff0000000000000ULL,
                                              0x7ff0000000000001ULL,
                                              0xfff0000000000001ULL,
                                              0x7ff8000000000042ULL,
                                              0xfff8000000000042ULL,
                                              0x80000000,
                                              0x7f800000,
                                              0xff800000,
                                              0x7f800001,
                                              0xff800001,
                                              0x7fc00042,
                                              0xffc00042,
                                              0x00800000,
                                              0x0010000000000000ULL,
                                              0x8000000000000001ULL};
    unsigned cases = 0;
    for (auto rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO})
      for (int stride : {-2, -1, 0, 1, 2})
        for (int count : {0, 1, 2, 3, 4, 5, 16, 31, 64, 129})
          for (int pattern = 0; pattern < 5; ++pattern) {
            std::array<Raw, 265> bits{};
            for (auto &value : bits)
              value = static_cast<Raw>(random());
            const int offset = stride < 0 ? 260 : 2;
            std::vector<Raw> logical;
            for (int i = 0; i < count; ++i) {
              Raw value = pattern == 0   ? static_cast<Raw>(random())
                          : pattern == 1 ? static_cast<Raw>(i)
                          : pattern == 2 ? static_cast<Raw>(count - i)
                          : pattern == 3 ? static_cast<Raw>(edges[i % edges.size()])
                                         : Raw(7);
              bits[offset + i * stride] = value;
            }
            for (int i = 0; i < count; ++i)
              logical.push_back(bits[offset + i * stride]);
            std::sort(logical.begin(), logical.end(), less);
            auto expected = bits;
            for (int i = 0; i < count; ++i)
              expected[offset + i * stride] = logical[i];
            alignas(16) std::array<Cell, 265> cells;
            std::memcpy(cells.data(), bits.data(), sizeof(cells));
            Array input{cells.data(), cells.data(), offset, {count}, {stride}};
            Invocation context;
#ifndef SORT_COPY
            context.failAllocationAt(1);
#endif
            require(std::fesetround(rounding) == 0, "rounding mode unavailable");
            std::feclearexcept(FE_ALL_EXCEPT);
#ifdef SORT_COPY
            Array output{};
            _mlir_ciface_kalandra_test_entry(&output, &input, &context);
            require(!context.status() && output.sizes[0] == count,
                    "sorted failed a valid descriptor");
            require(std::memcmp(cells.data(), bits.data(), sizeof(cells)) == 0,
                    "sorted modified its input");
            require(!count || output.allocated != cells.data(),
                    "sorted returned its input backing");
            for (int i = 0; i < count; ++i) {
              Raw actual;
              std::memcpy(&actual, output.aligned + output.offset + i * output.strides[0],
                          sizeof(Cell));
              require(actual == logical[i], "sorted lost raw bits or ordering");
            }
#else
            const auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
            require(!context.status() && result == count, "sort failed a valid descriptor");
            require(std::memcmp(cells.data(), expected.data(), sizeof(cells)) == 0,
                    "sort lost raw bits, numeric ordering, strides or neighboring storage");
            require(!context.allocations() && !context.live(), "in-place sort allocated storage");
#endif
            require(!std::fetestexcept(FE_ALL_EXCEPT) && std::fegetround() == rounding,
                    "sort changed FP environment or raised floating exceptions");
            context.settle();
            require(!context.live() && context.allocations() == context.releases(),
                    "sort leaked owners");
            ++cases;
          }
    for (auto mode : {0, 1, 2}) {
      std::array<Cell, 2> cells{Cell(2), Cell(1)}, original = cells;
      Array input{cells.data(), cells.data(), 0, {mode == 0 ? -1 : 2}, {1}};
      Invocation context;
      std::atomic<bool> stopped{mode == 1};
      context.bindCancellation(&stopped);
      if (mode == 2)
        context.fault(4, 17, 3);
#ifdef SORT_COPY
      Array output{};
      _mlir_ciface_kalandra_test_entry(&output, &input, &context);
      const auto result = output.allocated != nullptr;
#else
      auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
#endif
      require(!result &&
                  context.status() == (mode == 0   ? 1
                                       : mode == 2 ? 4
                                                   : 0) &&
                  std::memcmp(cells.data(), original.data(), sizeof(cells)) == 0 &&
                  !context.allocations(),
              "invalid/cancelled/faulted sort wrote storage");
      if (mode == 1)
        require(context.poll() == -1, "sort lost cancellation");
      if (mode == 2)
        require(context.line() == 17 && context.column() == 3, "fault precedence lost");
      context.settle();
    }
#ifdef SORT_COPY
    {
      std::array<Cell, 3> cells{Cell(3), Cell(1), Cell(2)}, before = cells;
      Array input{cells.data(), cells.data(), 0, {3}, {1}}, output{};
      Invocation context;
      context.failAllocationAt(1);
      _mlir_ciface_kalandra_test_entry(&output, &input, &context);
      require(context.status() == 2 && !output.allocated &&
                  std::memcmp(cells.data(), before.data(), sizeof(cells)) == 0,
              "failed sorted allocation modified its input or published output");
      context.settle();
      require(!context.live(), "failed sorted leaked storage");
    }
#endif
    std::fesetenv(&saved);
    std::cout << "Array sort: " << cases
              << " raw-bit/stride/pattern/FP-mode cases, no allocations, bounds/fault/cancel "
                 "checks passed\n";
  } catch (const std::exception &error) {
    std::fesetenv(&saved);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
