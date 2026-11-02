#include "../runtime/Runtime.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
struct Budget {
  Invocation *context;
  std::uint64_t used = 0, peakLive = 0;
  static bool reserve(void *p, std::uint64_t bytes) noexcept {
    auto &b = *static_cast<Budget *>(p);
    b.used += bytes;
    b.peakLive = std::max(b.peakLive, b.context->live() + 1);
    return true;
  }
  static void release(void *p, std::uint64_t bytes) noexcept {
    static_cast<Budget *>(p)->used -= bytes;
  }
};
int main() {
  try {
    Invocation context;
    Budget budget{&context};
    // Keep the existing cumulative reservation contract. Observe actual live
    // owners at each allocation, rather than pretending charges are refunded.
    context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    auto result = _mlir_ciface_kalandra_test_entry(10000, &context);
    require(result == 20012 && !context.status() && budget.peakLive <= 4 && !context.live(),
            "varying-length replacements grew live backing or lost a saved alias");
    context.settle();
    require(!budget.used && context.allocations() == context.releases(),
            "array replacement leaked reservation or ownership");
    for (unsigned failure = 1; failure < 45; ++failure) {
      Invocation c;
      c.failAllocationAt(failure);
      auto value = _mlir_ciface_kalandra_test_entry(20, &c);
      require(!c.status() || (c.status() == 2 && !value), "allocation fault/result changed");
      c.settle();
      require(!c.live() && c.allocations() == c.releases(), "failed replacement leaked backing");
    }
    {
      Invocation c;
      Budget observed{&c};
      c.bindRecursiveBudget(&observed, Budget::reserve, Budget::release);
      std::atomic<bool> stopped{false};
      std::atomic<std::uint64_t> polls{0};
      c.bindCancellation(&stopped);
      c.watchPolls(&polls);
      std::int64_t value = -1;
      std::jthread worker([&] { value = _mlir_ciface_kalandra_test_entry(INT64_MAX, &c); });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (polls.load(std::memory_order_acquire) < 64 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      const bool running = polls.load(std::memory_order_acquire) >= 64;
      stopped.store(true, std::memory_order_release);
      worker.join();
      require(running && !value && !c.status() && c.poll() == -1 && observed.peakLive <= 4,
              "running replacement loop lost cancellation or grew live owners");
      c.settle();
      require(!c.live() && !observed.used && c.allocations() == c.releases(),
              "cancelled replacement loop leaked owners/reservations");
    }
    for (auto fault : {false, true}) {
      Invocation c;
      std::atomic<bool> stopped{true};
      c.bindCancellation(&stopped);
      if (fault) c.fault(4, 17, 3);
      require(!_mlir_ciface_kalandra_test_entry(100, &c) && !c.allocations() &&
                  c.status() == (fault ? 4 : 0), "entry cancellation/fault precedence changed");
      c.settle();
    }
    std::cout << "10000 varying-length replacements: peak live owners " << budget.peakLive
              << ", saved alias, allocation faults/cancellation and cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
