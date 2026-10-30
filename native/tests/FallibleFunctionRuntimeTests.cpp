#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *data;
  std::int64_t offset, size, stride;
};
struct Error {
  std::int64_t tag;
  StringDescriptor reason;
  Descriptor numbers;
};
struct Result {
  Failure failure;
  std::int64_t value;
  Error error;
};
extern "C" void (*const kalandra_callable_test_entry)(Result *, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    require(kalandra_callable_test_entry, "private synchronous entry missing");
    std::uint64_t allocations = 0;
    unsigned cases = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation context;
      context.failAllocationAt(attempt);
      std::atomic<std::uint64_t> polls{0};
      context.watchPolls(&polls);
      Result result{};
      std::array<std::byte, sizeof(Result)> before;
      std::memcpy(before.data(), &result, sizeof(result));
      kalandra_callable_test_entry(&result, &context);
      ++cases;
      if (!attempt)
        allocations = context.allocations();
      const bool failed = attempt && attempt <= allocations;
      require(allocations < 64 && polls.load() >= 1, "unbounded allocation or unchecked entry");
      if (failed) {
        require(context.status() == 2 && !std::memcmp(&result, before.data(), sizeof(result)),
                "failed synchronous call published a partial outcome");
      } else {
#ifdef KALANDRA_EXPECT_FAULT
        require(context.status() == 4 && context.line() == 9 && context.column() == 3,
                "synchronous indirect fault lost its original source location");
#else
        require(context.status() == 0 && result.failure.kind == TaskFailure::None,
                "expected error became an execution or task failure");
#ifdef KALANDRA_EXPECT_SUCCESS
        require(result.value == 42 && result.error.tag == 0, "success channel changed");
#else
        require(result.error.tag == 2 && !result.value &&
                    validateString(context, result.error.reason),
                "typed error variant or text changed");
        require(result.error.reason.byteLength == 6 && result.error.numbers.size == 2 &&
                    result.error.numbers.offset == 0 && result.error.numbers.stride == 1,
                "typed error descriptor changed");
        auto text = context.lease(result.error.reason.allocated);
        auto numbers = context.lease(result.error.numbers.allocated);
        require(text && numbers, "error payload lost its managed owners");
        context.settle();
        require(!std::memcmp(result.error.reason.data, "é🙂", 6) &&
                    static_cast<const std::int64_t *>(result.error.numbers.data)[0] == 20 &&
                    static_cast<const std::int64_t *>(result.error.numbers.data)[1] == 22,
                "retained error payload did not survive invocation settlement");
#endif
#endif
      }
      context.settle();
      require(context.live() == 0 && context.allocations() == context.releases(),
              "synchronous frame or payload storage leaked");
    }
    Invocation alreadyFailed;
    alreadyFailed.fault(1, 99, 12);
    Result untouched;
    std::memset(&untouched, 0x5a, sizeof(untouched));
    std::array<std::byte, sizeof(Result)> snapshot;
    std::memcpy(snapshot.data(), &untouched, sizeof(untouched));
    kalandra_callable_test_entry(&untouched, &alreadyFailed);
    require(alreadyFailed.status() == 1 && alreadyFailed.line() == 99 &&
                alreadyFailed.column() == 12 && alreadyFailed.allocations() == 0 &&
                !std::memcmp(&untouched, snapshot.data(), sizeof(untouched)),
            "failed context entered a synchronous callable or replaced its primary fault");
    std::atomic<bool> stop{true};
    Invocation cancelled;
    cancelled.bindCancellation(&stop);
    kalandra_callable_test_entry(&untouched, &cancelled);
    require(cancelled.status() == 0 && cancelled.allocations() == 0 &&
                !std::memcmp(&untouched, snapshot.data(), sizeof(untouched)),
            "pre-cancelled context entered a synchronous callable");
    std::cout << "Synchronous callable: " << cases
              << " allocation probes; typed outcomes, retained payloads, faults and cancellation "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
