#include "../runtime/Tensor.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
using Array = CpuTensor<NumericType::Int64, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, std::int64_t, Invocation *);
int main() {
  try {
    std::array<std::int64_t, 1> cells{-1};
    Array input{cells.data(), cells.data(), 0, {1}, {1}};
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> polls{0};
    Invocation context;
    context.bindCancellation(&stopped);
    context.watchPolls(&polls);
    std::int64_t result = -1;
    std::jthread worker(
        [&] { result = _mlir_ciface_kalandra_test_entry(&input, INT64_MAX, &context); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (polls.load(std::memory_order_acquire) < 64 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool observed = polls.load(std::memory_order_acquire) >= 64;
    stopped.store(true, std::memory_order_release);
    worker.join();
    if (!observed || context.poll() != -1 || context.status() || result || cells[0] < 0 ||
        cells[0] == INT64_MAX || context.allocations())
      throw std::runtime_error(
          "running mutation loop lost cancellation, partial-write semantics, or allocation bounds");
    context.settle();
    if (context.live())
      throw std::runtime_error("cancelled mutation loop leaked ownership");
    Invocation prior;
    prior.fault(4, 17, 3);
    prior.bindCancellation(&stopped);
    auto before = cells;
    _mlir_ciface_kalandra_test_entry(&input, 10, &prior);
    if (prior.status() != 4 || prior.line() != 17 || cells != before)
      throw std::runtime_error("cancellation replaced a prior fault or wrote after it");
    std::cout << "Mutation loop: running cancellation, visible completed writes, no allocations "
                 "and prior-fault precedence passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
