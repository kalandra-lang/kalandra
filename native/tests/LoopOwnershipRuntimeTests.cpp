#include "../runtime/Runtime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using kalandra::runtime::Invocation;
extern "C" std::int64_t (*const kalandra_callable_test_entry)(std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Budget {
  Invocation *context;
  std::uint64_t used = 0, peak = 0;
  static bool reserve(void *value, std::uint64_t bytes) noexcept {
    auto &self = *static_cast<Budget *>(value);
    self.used += bytes;
    self.peak = std::max(self.peak, self.context->live() + 1);
    return true;
  }
  static void release(void *value, std::uint64_t bytes) noexcept {
    static_cast<Budget *>(value)->used -= bytes;
  }
};
int main() {
  try {
    for (auto count : {0, 1, 20, 1000}) {
      Invocation context;
      Budget budget{&context};
      context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
      std::int64_t expected = 7;
      for (int i = 0; i < count; ++i)
        if (i % 2)
          expected += i % 17;
      auto value = kalandra_callable_test_entry(count, &context);
      require(value == expected && !context.status() && budget.peak <= 32 && !context.live(),
              "loop exit lost an array or retained backing across iterations");
      context.settle();
      require(!budget.used && context.allocations() == context.releases(),
              "loop leaked reservation");
    }
    for (unsigned failure = 1; failure <= 64; ++failure) {
      Invocation context;
      context.failAllocationAt(failure);
      auto value = kalandra_callable_test_entry(20, &context);
      require(!context.status() || (context.status() == 2 && !value),
              "allocation failure published a loop result");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "failed loop leaked ownership");
    }
    Invocation context;
    Budget budget{&context};
    context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> polls{0};
    context.bindCancellation(&stopped);
    context.watchPolls(&polls);
    std::int64_t value = -1;
    std::jthread worker([&] { value = kalandra_callable_test_entry(INT64_MAX, &context); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (polls.load(std::memory_order_acquire) < 64 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool running = polls.load(std::memory_order_acquire) >= 64;
    stopped.store(true, std::memory_order_release);
    worker.join();
    require(running && !value && !context.status() && context.poll() < 0 && budget.peak <= 32,
            "owned loop lost cancellation or bounded storage");
    context.settle();
    require(!context.live() && !budget.used && context.allocations() == context.releases(),
            "cancelled loop leaked owners or reservation");
    std::cout << "Loop exits: bounded owned backing, 64 allocation failures, cancellation and "
                 "cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
