#include "../runtime/Runtime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using kalandra::runtime::Invocation;
extern "C" std::int64_t (*const kalandra_callable_test_entry)(std::int64_t, std::int64_t,
                                                              Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Budget {
  Invocation *context;
  std::uint64_t used = 0, peakOwners = 0;
  static bool reserve(void *value, std::uint64_t bytes) noexcept {
    auto &self = *static_cast<Budget *>(value);
    self.used += bytes;
    self.peakOwners = std::max(self.peakOwners, self.context->live() + 1);
    return true;
  }
  static void release(void *value, std::uint64_t bytes) noexcept {
    static_cast<Budget *>(value)->used -= bytes;
  }
};
int main() {
  try {
    for (auto count : {0, 1, 16, 17, 1025, 10000}) {
      Invocation context;
      Budget budget{&context};
      context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
      const auto value = kalandra_callable_test_entry(count, -1, &context);
      require(value == count && !context.status() && !context.live() && budget.peakOwners <= 32,
              "collection lost elements, retained a builder or accumulated old growth buffers");
      context.settle();
      require(!budget.used && context.allocations() == context.releases(),
              "collection leaked budget");
    }
    for (unsigned failure = 1; failure <= 96; ++failure) {
      Invocation context;
      context.failAllocationAt(failure);
      const auto value = kalandra_callable_test_entry(65, -1, &context);
      require(context.status() ? context.status() == 2 && !value : value == 65,
              "allocation failure published a partial collection");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "failed collection leaked");
    }
    for (auto position : {0, 16, 64}) {
      Invocation context;
      const auto value = kalandra_callable_test_entry(65, position, &context);
      require(!value && context.status() > 0, "checked fault published a partial array");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "faulted collection leaked");
    }
    Invocation context;
    Budget budget{&context};
    context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> polls{0};
    context.bindCancellation(&stopped);
    context.watchPolls(&polls);
    std::int64_t value = -1;
    std::jthread worker([&] { value = kalandra_callable_test_entry(INT64_MAX, -1, &context); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (polls.load(std::memory_order_acquire) < 500 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool running = polls.load(std::memory_order_acquire) >= 500;
    stopped.store(true, std::memory_order_release);
    worker.join();
    require(running && !value && !context.status() && context.poll() < 0,
            "collection lost cancellation");
    context.settle();
    require(!context.live() && !budget.used && context.allocations() == context.releases(),
            "cancelled collection leaked backing or control storage");
    std::cout << "Yield collections: growth, bounded live buffers, 96 allocation failures, checked "
                 "faults and running cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
