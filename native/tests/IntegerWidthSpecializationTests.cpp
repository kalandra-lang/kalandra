#include "../runtime/Runtime.h"
#include "../runtime/NativeCancellation.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t,std::int64_t,Invocation *);
int main() {
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  constexpr std::array<std::int64_t,18> seeds{0,1,2,-1,32767,32768,65534,65535,65536,
      INT32_MIN,INT32_MAX,min/65536,min/65536+32768,min/65536+32769,
      max/65536-1,max/65536,max/65536+1,max};
  constexpr std::array<std::int64_t,10> denominators{0,1,2,3,65534,65535,65536,-1,min,max};
  const bool interrupts = initializeNativeCancellation();
  unsigned count = 0;
  for (int mode = 0; mode < 4; ++mode) for (auto seed : seeds) for (auto denominator : denominators) {
    std::int64_t product = 0, numerator = 0, divisor = 0, expected = 0;
    bool fault = __builtin_mul_overflow(seed,std::int64_t(65536),&product) ||
        __builtin_sub_overflow(product,std::int64_t(2147483648),&numerator) ||
        __builtin_sub_overflow(denominator,std::int64_t(1),&divisor) || !divisor;
    if (!fault) {
#ifdef REMAINDER
      expected = numerator == min && divisor == -1 ? 0 : numerator % divisor;
#else
      fault = numerator == min && divisor == -1;
      if (!fault) expected = numerator / divisor;
#endif
    }
    Invocation context; std::atomic<bool> token{false}; std::atomic<std::uint64_t> polls{0};
    if (mode) context.bindCancellation(&token);
    if (mode == 2) context.watchPolls(&polls);
    if (mode == 3) context.allowNativeInterrupts(interrupts);
    const auto result = _mlir_ciface_kalandra_test_entry(seed,denominator,&context);
    if (bool(context.status()) != fault ||
        std::bit_cast<std::uint64_t>(result) != std::bit_cast<std::uint64_t>(double(expected))) {
      std::cerr << "seed=" << seed << " denominator=" << denominator << " mode=" << mode
                << " status=" << context.status() << " result=" << result << " expected=" << expected << '\n';
      throw std::runtime_error("integer width specialization changed division semantics");
    }
    context.settle();
    if (context.live() || context.callDepth() || context.allocations() != context.releases())
      throw std::runtime_error("integer width specialization leaked invocation state");
    ++count;
  }
  std::cout << count << " exact integer width/division boundary cases passed\n";
}
