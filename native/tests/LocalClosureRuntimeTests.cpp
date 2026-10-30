#include "../runtime/NativeCancellation.h"
#include "../runtime/Runtime.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t,std::int64_t,Invocation *);
static void check(bool ok) {
  if (!ok) throw std::runtime_error("local closure execution/ownership regression");
}
int main() {
  const bool interrupts = initializeNativeCancellation();
  for (int mode = 0; mode != 4; ++mode)
    for (std::int64_t seed : {1,17,31})
      for (std::int64_t rounds : {0,1,16,256,100000}) {
        Invocation context;
        context.failAllocationAt(0);
        std::atomic<bool> token{false};
        std::atomic<std::uint64_t> polls{0};
        if (mode) context.bindCancellation(&token);
        if (mode == 2) context.watchPolls(&polls);
        if (mode == 3) context.allowNativeInterrupts(interrupts);
        std::int64_t expected = seed;
        for (std::int64_t n = 0; n < rounds; ++n)
          expected = (expected*13+seed+7)%1000003;
        check(_mlir_ciface_kalandra_test_entry(seed,rounds,&context) == expected);
        check(!context.status() && !context.callDepth());
        // Clear native execution must eliminate the locally consumed capture
        // allocation, including after late graph inlining. Observed/cooperative
        // workers may conservatively retain their owned environment.
        if (mode == 0 || (mode == 3 && interrupts)) check(context.allocations() == 0);
        if (mode == 2) check(polls.load() != 0);
        context.settle();
        check(!context.live() && context.allocations() == context.releases());
      }
  for (auto seed : {std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::int64_t>::max()}) {
    Invocation context;
    check(_mlir_ciface_kalandra_test_entry(seed,16,&context) == 0 && context.status() == 4);
    context.settle();
    check(!context.callDepth() && !context.live() && context.allocations() == context.releases());
  }
  std::cout << "Local closures: 60 exact oracle/mode cases; clear-worker allocation elimination, "
               "checked overflow, observed polls and settled ownership passed\n";
}
