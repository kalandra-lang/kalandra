#include "../runtime/NativeCancellation.h"
#include "../runtime/Runtime.h"
#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
struct Expected {
  std::int64_t value;
  std::int64_t line = 0, column = 0;
};
static Expected oracle(std::int64_t seed, std::int64_t rounds) {
  std::int64_t offset;
  if (__builtin_add_overflow(seed, std::int64_t(7), &offset)) return {0,21,29};
  auto value = seed;
  while (rounds) {
    std::int64_t product, sum;
    if (__builtin_mul_overflow(value, std::int64_t(13), &product)) return {0,12,15};
    if (__builtin_add_overflow(product, offset, &sum)) return {0,12,14};
    value = sum % 1000003;
    if (__builtin_sub_overflow(rounds, std::int64_t(1), &rounds)) return {0,18,44};
  }
  return {value};
}
int main() {
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  std::vector<std::int64_t> seeds{0,1,-1,17,-17,min,max};
  for (auto value : std::array<std::int64_t,14>{-32769,-32768,-32767,-1,0,1,
          32766,32767,32768,32769,INT32_MIN,std::int64_t(INT32_MIN)+1,
          INT32_MAX,std::int64_t(INT32_MAX)+1}) seeds.push_back(value);
  for (auto boundary : {min,max,min/13,max/13,min/14,max/14})
    for (std::int64_t delta = -16; delta <= 16; ++delta) {
      std::int64_t value;
      if (!__builtin_add_overflow(boundary,delta,&value)) seeds.push_back(value);
    }
  std::uint64_t state = 0xcafed1234853;
  for (unsigned n = 0; n < 512; ++n) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    seeds.push_back(std::bit_cast<std::int64_t>(state));
  }
  const bool interrupts = initializeNativeCancellation();
  unsigned checked = 0;
  for (int mode = 0; mode < 5; ++mode)
    for (auto seed : seeds)
      for (std::int64_t rounds : std::array<std::int64_t,12>{0,1,2,16,22,23,24,256,257,min,min+1,min+2}) {
        const auto expected = oracle(seed,rounds);
        Invocation context;
        context.failAllocationAt(0);
        std::atomic<bool> token{false}; std::atomic<std::uint64_t> polls{0};
        if (mode) context.bindCancellation(&token);
        if (mode == 2) context.watchPolls(&polls);
        if (mode == 3) context.allowNativeInterrupts(interrupts);
        if (mode == 4 && !context.enterCall(0,0))
          throw std::runtime_error("nested fixture could not enter");
        const auto result = _mlir_ciface_kalandra_test_entry(seed,rounds,&context);
        const auto wanted = static_cast<double>(expected.value);
        if (context.status() != (expected.line ? 4 : 0) ||
            (expected.line && (context.line() != expected.line || context.column() != expected.column)) ||
            std::bit_cast<std::uint64_t>(result) != std::bit_cast<std::uint64_t>(wanted)) {
          std::cerr << "mode=" << mode << " seed=" << seed << " rounds=" << rounds
                    << " status=" << context.status() << " at " << context.line() << ':'
                    << context.column() << " expected " << expected.line << ':' << expected.column << '\n';
          throw std::runtime_error("captured integer range/value/fault-order regression");
        }
        if (mode == 2 && !polls.load())
          throw std::runtime_error("watched invocation lost its polls");
        if (mode == 4) {
          if (context.callDepth() != 1)
            throw std::runtime_error("nested invocation lost its entry depth");
          context.leaveCall();
        }
        context.settle();
        if (context.live() || context.callDepth() || context.allocations() != context.releases())
          throw std::runtime_error("captured integer ownership regression");
        ++checked;
      }
  for (bool native : {false,true}) {
    Invocation context; std::atomic<bool> stopped{true};
    context.bindCancellation(&stopped); context.allowNativeInterrupts(native && interrupts);
    if (_mlir_ciface_kalandra_test_entry(17,16,&context) != 0 || context.status() || context.callDepth())
      throw std::runtime_error("bounded entry ignored an already cancelled token");
  }
  std::cout << checked << " captured integer cases: exact values, overflow boundaries, "
               "fault locations/order and execution modes passed\n";
}
