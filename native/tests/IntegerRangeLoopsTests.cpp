#include "PoisonedInvocation.h"
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
  constexpr std::array<std::int64_t,17> seeds{0,1,2,17,-1,-17,32767,32768,65534,65535,
      65536,65537,1000002,1000003,min,max/13,max};
  constexpr std::array<std::int64_t,10> counts{0,1,2,3,16,23,24,257,min,min+1};
  const bool interrupts = initializeNativeCancellation(); unsigned checked = 0;
  for (int mode = 0; mode < 4; ++mode) for (auto seed : seeds) for (auto rounds : counts) {
    std::int64_t value = CASE == 1 ? -1 : seed;
    bool fault = CASE == 4 && __builtin_mul_overflow(seed,std::int64_t(65536),&value);
    auto remaining = rounds;
    while (!fault && remaining) {
      std::int64_t product = 0, sum = 0;
      const std::int64_t coefficient = CASE == 2 ? -13 : CASE == 6 ? max : 13;
      const std::int64_t bias = CASE == 3 ? -65536 : 7;
      fault = __builtin_mul_overflow(value,coefficient,&product) ||
              __builtin_add_overflow(product,bias,&sum);
      if (!fault) {
        value = sum % (CASE == 5 ? -1000003 : 1000003);
        fault = __builtin_sub_overflow(remaining,std::int64_t(1),&remaining);
      }
    }
    PoisonedInvocation ownedContext; Invocation &context = ownedContext.get(); std::atomic<bool> token{false}; std::atomic<std::uint64_t> polls{0};
    if (mode) context.bindCancellation(&token);
    if (mode == 2) context.watchPolls(&polls);
    if (mode == 3) context.allowNativeInterrupts(interrupts);
    const auto result = _mlir_ciface_kalandra_test_entry(seed,rounds,&context);
    const auto expected = fault ? 0. : double(value);
    if (context.status() != (fault ? 4 : 0) ||
        std::bit_cast<std::uint64_t>(result) != std::bit_cast<std::uint64_t>(expected)) {
      std::cerr << "case=" << CASE << " seed=" << seed << " rounds=" << rounds
                << " mode=" << mode << " result=" << result << " expected=" << expected
                << " status=" << context.status() << '\n';
      throw std::runtime_error("integer recurrence range proof changed source arithmetic");
    }
    context.settle();
    if (context.live() || context.callDepth() || context.allocations() != context.releases())
      throw std::runtime_error("integer recurrence range proof leaked invocation state");
    ++checked;
  }
  std::cout << checked << " exact signed recurrence and range-fallback cases passed\n";
}
