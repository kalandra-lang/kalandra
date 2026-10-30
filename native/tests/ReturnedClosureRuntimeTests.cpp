#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
struct Closure {
  std::int64_t (*code)(void *, std::int64_t, Invocation *);
  void *environment;
};
extern "C" void _mlir_ciface_kalandra_test_entry(Closure *, std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    Invocation context;
    Closure first{}, second{};
    _mlir_ciface_kalandra_test_entry(&first, 10, &context);
    _mlir_ciface_kalandra_test_entry(&second, 20, &context);
    require(context.status() == 0 && first.code && second.code && first.environment &&
                second.environment && first.environment != second.environment,
            "creators failed or shared a mutable capture environment");
    for (int i = 0; i < 32; ++i) {
      require(first.code(first.environment, i, &context) == 42 + i,
              "returned closure lost captured record after creator exit");
      require(second.code(second.environment, -i, &context) == 52 - i,
              "second closure changed the first environment");
    }
    require(context.status() == 0, "repeated closure calls failed");
    const auto beforeStop = context.allocations();
    std::atomic<bool> stopCall{true};
    context.bindCancellation(&stopCall);
    require(first.code(first.environment, 0, &context) == 0 && context.status() == 0 &&
                context.allocations() == beforeStop,
            "cancelled invocation entered captured closure body");
    stopCall.store(false);
    context.fault(1, 88, 9);
    require(first.code(first.environment, 0, &context) == 0 && context.status() == 1 &&
                context.line() == 88 && context.column() == 9 &&
                context.allocations() == beforeStop,
            "failed invocation entered captured closure body or replaced primary fault");
    context.settle();
    require(context.live() == 0 && context.allocations() == context.releases(),
            "returned closure retained storage after settlement");
    std::uint64_t allocations = 0;
    unsigned probes = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation probe;
      probe.failAllocationAt(attempt);
      Closure result;
      std::memset(&result, 0x5a, sizeof(result));
      std::array<std::byte, sizeof(Closure)> before;
      std::memcpy(before.data(), &result, sizeof(result));
      _mlir_ciface_kalandra_test_entry(&result, 10, &probe);
      ++probes;
      if (!attempt)
        allocations = probe.allocations();
      require(allocations < 128, "unbounded environment allocation");
      if (attempt && attempt <= allocations)
        require(probe.status() == 2 && !std::memcmp(before.data(), &result, sizeof(result)),
                "failed creation published a partial closure");
      else
        require(probe.status() == 0 && result.code && result.environment,
                "valid closure creation failed");
      probe.settle();
      require(probe.live() == 0 && probe.allocations() == probe.releases(),
              "failed closure creation leaked captured storage");
    }
    Closure untouched;
    std::memset(&untouched, 0x5a, sizeof(untouched));
    std::array<std::byte, sizeof(Closure)> before;
    std::memcpy(before.data(), &untouched, sizeof(untouched));
    Invocation fault;
    fault.fault(1, 99, 12);
    _mlir_ciface_kalandra_test_entry(&untouched, 10, &fault);
    require(fault.status() == 1 && fault.line() == 99 && fault.column() == 12 &&
                !fault.allocations() && !std::memcmp(before.data(), &untouched, sizeof(untouched)),
            "faulted context published a closure");
    std::atomic<bool> stop{true};
    Invocation cancelled;
    cancelled.bindCancellation(&stop);
    _mlir_ciface_kalandra_test_entry(&untouched, 10, &cancelled);
    require(!cancelled.status() && !cancelled.allocations() &&
                !std::memcmp(before.data(), &untouched, sizeof(untouched)),
            "cancelled context published a closure");
    std::cout << "Returned closure: 64 calls after creator exit; " << probes
              << " creation allocation probes and settled captures passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
