#include "../runtime/Runtime.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#ifndef KALANDRA_EXPECTED
#define KALANDRA_EXPECTED 42
#endif
using kalandra::runtime::Invocation;
extern "C" std::int64_t (*const kalandra_callable_test_entry)(Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::uint64_t allocations = 0;
    unsigned probes = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation context;
      context.failAllocationAt(attempt);
      auto result = kalandra_callable_test_entry(&context);
      ++probes;
      if (!attempt)
        allocations = context.allocations();
      require(allocations < 128, "unbounded capture allocation");
      if (attempt && attempt <= allocations)
        require(context.status() == 2 && result == 0,
                "allocation fault published partial map callback output");
      else
        require(context.status() == 0 && result == KALANDRA_EXPECTED,
                "map callback call failed after creator returned");
      context.settle();
      require(context.live() == 0 && context.allocations() == context.releases(),
              "map callback environment or captured payload leaked");
    }
    Invocation fault;
    fault.fault(1, 99, 12);
    require(kalandra_callable_test_entry(&fault) == 0 && fault.status() == 1 &&
                fault.line() == 99 && fault.column() == 12 && fault.allocations() == 0,
            "faulted context entered map callback creator");
    std::atomic<bool> stop{true};
    Invocation cancelled;
    cancelled.bindCancellation(&stop);
    require(kalandra_callable_test_entry(&cancelled) == 0 && cancelled.status() == 0 &&
                cancelled.allocations() == 0,
            "cancelled context entered map callback creator");
    std::cout << "Map callback context: " << probes
              << " allocation probes; settled ownership and stopped contexts passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
