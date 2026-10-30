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
struct Functions {
  void *allocated;
  Closure *data;
  std::int64_t offset, size, stride;
};
extern "C" void _mlir_ciface_kalandra_test_entry(Functions *, std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void validate(Invocation &context, const Functions &value) {
  kalandra_record_array_validate_v1(&context, &value, sizeof(Closure), alignof(Closure));
  require(context.status() == 0 && value.allocated && value.data && value.size == 2,
          "function array descriptor lost its managed storage");
  for (unsigned i = 0; i < 2; ++i)
    require(value.data[i].code && value.data[i].environment,
            "function array element lost its code/environment pair");
}
int main() {
  try {
    Invocation context;
    Functions first{}, second{};
    _mlir_ciface_kalandra_test_entry(&first, 10, &context);
    _mlir_ciface_kalandra_test_entry(&second, 20, &context);
    validate(context, first);
    validate(context, second);
    require(first.allocated != second.allocated &&
                first.data[0].environment != second.data[0].environment,
            "creators shared mutable array/capture storage");
    for (int i = 0; i < 32; ++i) {
      require(first.data[0].code(first.data[0].environment, i, &context) == 42 + i &&
                  first.data[1].code(first.data[1].environment, i, &context) == 10 + i &&
                  second.data[0].code(second.data[0].environment, -i, &context) == 52 - i &&
                  second.data[1].code(second.data[1].environment, -i, &context) == 20 - i,
              "returned function array lost captured values after creator exit");
    }
    require(context.status() == 0, "repeated function-array invocation failed");
    const auto beforeStop = context.allocations();
    std::atomic<bool> stopCall{true};
    context.bindCancellation(&stopCall);
    require(first.data[0].code(first.data[0].environment, 0, &context) == 0 &&
                context.status() == 0 && context.allocations() == beforeStop,
            "cancelled context entered array callback");
    stopCall.store(false);
    context.fault(1, 88, 9);
    require(first.data[0].code(first.data[0].environment, 0, &context) == 0 &&
                context.status() == 1 && context.line() == 88 && context.column() == 9 &&
                context.allocations() == beforeStop,
            "failed context entered array callback or replaced the primary fault");
    context.settle();
    require(!context.live() && context.allocations() == context.releases(),
            "function array or transitive captures leaked after settlement");
    std::uint64_t allocations = 0;
    unsigned probes = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation probe;
      probe.failAllocationAt(attempt);
      Functions result;
      std::memset(&result, 0x5a, sizeof(result));
      std::array<std::byte, sizeof(Functions)> before;
      std::memcpy(before.data(), &result, sizeof(result));
      _mlir_ciface_kalandra_test_entry(&result, 10, &probe);
      ++probes;
      if (!attempt)
        allocations = probe.allocations();
      require(allocations < 128, "unbounded function-array creation allocation");
      if (attempt && attempt <= allocations)
        require(probe.status() == 2 && !std::memcmp(before.data(), &result, sizeof(result)),
                "failed creation published a partial function array");
      else
        validate(probe, result);
      probe.settle();
      require(!probe.live() && probe.allocations() == probe.releases(),
              "failed array creation leaked backing or captured payloads");
    }
    for (bool cancelled : {false, true}) {
      Invocation stopped;
      std::atomic<bool> stop{true};
      if (cancelled)
        stopped.bindCancellation(&stop);
      else
        stopped.fault(1, 99, 12);
      Functions result;
      std::memset(&result, 0x5a, sizeof(result));
      std::array<std::byte, sizeof(Functions)> before;
      std::memcpy(before.data(), &result, sizeof(result));
      _mlir_ciface_kalandra_test_entry(&result, 10, &stopped);
      require(stopped.status() == (cancelled ? 0 : 1) && !stopped.allocations() &&
                  !std::memcmp(before.data(), &result, sizeof(result)),
              "stopped creator allocated or published an array");
    }
    std::cout << "Function array: 128 calls after creator exit; " << probes
              << " creation allocation probes and settled transitive captures passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
