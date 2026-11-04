#include "../runtime/Runtime.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using kalandra::runtime::Invocation;
#ifdef KALANDRA_LOOP_CALLABLE_PROBE
extern "C" std::int64_t (*const kalandra_callable_test_entry)(std::int64_t, std::int64_t,
                                                              Invocation *);
static std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t count, std::int64_t faultAt,
                                                     Invocation *context) {
  return kalandra_callable_test_entry(count, faultAt, context);
}
#else
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
#endif
static void require(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
int main() {
  try {
    for (std::int64_t n : {0, 1, 17, 100000}) {
      Invocation context;
      std::int64_t expected = 0;
      for (std::int64_t i = 0; i < n; ++i)
        expected = (expected + i) % 1000003;
      auto result = _mlir_ciface_kalandra_test_entry(n, -1, &context);
      require(!context.status() && result == expected, "loop result mismatch");
      require(!context.allocations(), "scalar loop allocated heap backing");
    }
    Invocation fault;
    _mlir_ciface_kalandra_test_entry(100, 17, &fault);
    require(fault.status() == 4, "loop lost checked arithmetic fault");
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint64_t> polls{0};
    Invocation context;
    context.bindCancellation(&cancelled);
    context.watchPolls(&polls);
    std::jthread worker([&] { _mlir_ciface_kalandra_test_entry(INT64_MAX, -1, &context); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (polls.load(std::memory_order_acquire) < 4 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool observed = polls.load(std::memory_order_acquire) >= 4;
    cancelled.store(true, std::memory_order_release);
    worker.join();
    require(observed && context.poll() < 0,
            "running loop did not preserve cancellation checkpoints");
    context.settle();
    require(!context.live(), "cancelled scalar loop leaked");
    Invocation prior;
    prior.fault(1, 7, 3);
    prior.bindCancellation(&cancelled);
    _mlir_ciface_kalandra_test_entry(100, -1, &prior);
    require(prior.status() == 1 && prior.line() == 7, "cancellation replaced prior fault");
    std::cout << "Scalar loops: exact results, no backing allocations, dynamic faults, running "
                 "cancellation and precedence passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
