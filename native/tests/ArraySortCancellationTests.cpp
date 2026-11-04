#include "../runtime/Tensor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using Array = CpuTensor<NumericType::Int64, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, Invocation *);
int main() {
  try {
    std::vector<std::int64_t> cells(1 << 20);
    std::mt19937_64 random(731);
    for (auto &value : cells)
      value = static_cast<std::int64_t>(random());
    auto original = cells;
    Array input{cells.data(), cells.data(), 0, {std::int64_t(cells.size())}, {1}};
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> polls{0};
    Invocation context;
    context.bindCancellation(&stopped);
    context.watchPolls(&polls);
    context.failAllocationAt(1);
    std::int64_t result = -1;
    std::jthread worker([&] { result = _mlir_ciface_kalandra_test_entry(&input, &context); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (polls.load(std::memory_order_acquire) < 64 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool observed = polls.load(std::memory_order_acquire) >= 64;
    stopped.store(true, std::memory_order_release);
    worker.join();
    if (!observed || context.poll() != -1 || context.status() || result || context.allocations())
      throw std::runtime_error("running sort lost cancellation or allocated auxiliary storage");
    std::sort(cells.begin(), cells.end());
    std::sort(original.begin(), original.end());
    if (cells != original)
      throw std::runtime_error("cancelled sort lost or duplicated elements");
    context.settle();
    if (context.live())
      throw std::runtime_error("cancelled sort leaked ownership");
    std::cout << "Running sort cancellation: bounded polling, element permutation and zero "
                 "allocations passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
