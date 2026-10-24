#include "../runtime/Runtime.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
extern "C" std::int64_t (*const kalandra_callable_test_entry)(std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    Invocation balanced;
    for (unsigned i = 0; i < 1000; ++i) {
      require(kalandra_call_enter_v1(&balanced, 1, 2), "ordinary call admission failed");
      kalandra_call_leave_v1(&balanced);
    }
    require(balanced.callDepth() == 0 && balanced.status() == 0, "admission counter leaked");
    for (unsigned i = 0; i < 10000; ++i)
      require(kalandra_call_enter_v1(&balanced, 1, 2), "manual entries retained a call cap");
    require(balanced.callDepth() == 10000 && balanced.status() == 0,
            "manual admission introduced an arbitrary recursion cap");
    for (unsigned i = 0; i < 10000; ++i)
      kalandra_call_leave_v1(&balanced);
    require(balanced.callDepth() == 0, "manual admission did not unwind");
    unsigned probes = 0;
    std::uint64_t allocations = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation context;
      context.failAllocationAt(attempt);
      const auto result = kalandra_callable_test_entry(20, &context);
      ++probes;
      if (!attempt)
        allocations = context.allocations();
      require(allocations < 512 && context.callDepth() == 0,
              "recursive call leaked admission or unbounded allocations");
      if (attempt && attempt <= allocations)
        require(context.status() == 2 && result == 0,
                "recursive allocation fault published output");
      else
        require(context.status() == 0 && result == 42, "recursive success/error recovery failed");
      context.settle();
      require(context.live() == 0 && context.allocations() == context.releases(),
              "recursive invocation leaked storage");
    }
    Invocation repeated;
    for (unsigned i = 0; i < 256; ++i)
      require(kalandra_callable_test_entry(20, &repeated) == 42 && repeated.callDepth() == 0 &&
                  repeated.status() == 0,
              "repeated recursive calls exhausted admission");
    repeated.settle();
    require(repeated.live() == 0 && repeated.allocations() == repeated.releases(),
            "repeated recursive calls leaked storage");
    Invocation deep;
    require(kalandra_callable_test_entry(1000000, &deep) == 0 && deep.status() == 6 &&
                deep.callDepth() == 0,
            "runaway recursion did not fail and unwind safely");
    deep.settle();
    require(deep.live() == 0 && deep.allocations() == deep.releases(),
            "runaway recursion leaked storage");
    Invocation arithmetic;
    require(kalandra_callable_test_entry(-1, &arithmetic) == 0 && arithmetic.status() == 4 &&
                arithmetic.callDepth() == 0,
            "recursive arithmetic fault lost its failure or admission cleanup");
    arithmetic.settle();
    require(arithmetic.live() == 0 && arithmetic.allocations() == arithmetic.releases(),
            "arithmetic failure leaked recursive storage");
    Invocation failed;
    failed.fault(4, 8, 9);
    require(kalandra_callable_test_entry(20, &failed) == 0 && failed.status() == 4 &&
                failed.line() == 8 && failed.column() == 9 && failed.callDepth() == 0 &&
                failed.allocations() == 0,
            "already-failed context entered recursive source code");
    std::atomic<bool> stop{true};
    Invocation cancelled;
    cancelled.bindCancellation(&stop);
    require(kalandra_callable_test_entry(20, &cancelled) == 0 && cancelled.status() == 0 &&
                cancelled.callDepth() == 0 && cancelled.allocations() == 0,
            "cancelled context entered recursive source code");
    std::cout
        << "Recursive context: " << probes
        << " allocation probes; 256 repeated calls, deep/arithmetic failure, stopped contexts and "
           "balanced admission passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
