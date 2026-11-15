#include "../runtime/Tensor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using Array = CpuTensor<NumericType::Int64, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    unsigned cases = 0;
    // Rank is value / 1000: low digits make stable tie order observable.
    for (int count : {0, 1, 2, 3, 17, 65, 257})
      for (int stride : {-2, -1, 1, 2}) {
        std::vector<std::int64_t> cells(2 * count + 8, -77);
        const int offset = stride < 0 ? 2 * count + 2 : 2;
        std::vector<std::int64_t> expected;
        for (int i = 0; i < count; ++i) {
          auto value = ((i * 31 + 7) % 17) * 1000 + i;
          cells[offset + i * stride] = value;
          expected.push_back(value);
        }
        auto original = cells;
        std::stable_sort(expected.begin(), expected.end(),
                         [](auto a, auto b) { return a / 1000 < b / 1000; });
        Array input{cells.data(), cells.data(), offset, {count}, {stride}};
        Invocation context;
        auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
        require(result == count && !context.status(), "sort result/fault mismatch");
        for (int i = 0; i < count; ++i)
          original[offset + i * stride] = expected[i];
        require(cells == original, "stable sort changed order, stride or neighboring cells");
        context.settle();
        require(!context.live(), "sort leaked storage");
        ++cases;
      }
    for (unsigned failure = 1; failure < 16; ++failure) {
      std::vector<std::int64_t> cells{3001, 1002, 3003, 2004, 1005};
      const auto original = cells;
      Array input{cells.data(), cells.data(), 0, {5}, {1}};
      Invocation context;
      context.failAllocationAt(failure);
      const auto result = _mlir_ciface_kalandra_test_entry(&input, &context);
      if (context.status())
        require(!result && cells == original, "allocation fault partially mutated input");
      else
        require(result == 5 && cells == std::vector<std::int64_t>({1002, 1005, 2004, 3001, 3003}),
                "post-allocation-failure sort mismatch");
      const bool success = !context.status();
      context.settle();
      require(!context.live(), "allocation-failed sort leaked storage");
      ++cases;
      if (success)
        break;
      require(failure != 15, "allocation sweep did not reach success");
    }
    {
      std::vector<std::int64_t> cells{-3001, -1002, -3003};
      const auto original = cells;
      Array input{cells.data(), cells.data(), 0, {3}, {1}};
      Invocation context;
      require(!_mlir_ciface_kalandra_test_entry(&input, &context) && context.status() &&
                  cells == original,
              "comparator fault changed input or disappeared");
      context.settle();
      require(!context.live(), "comparator fault leaked storage");
      ++cases;
    }
    for (std::uint64_t stop : {0, 64, 4096, 32768}) {
      std::vector<std::int64_t> cells(4096);
      std::mt19937_64 random(731);
      for (auto &cell : cells)
        cell = random() % 1000000;
      auto expected = cells;
      Array input{cells.data(), cells.data(), 0, {4096}, {1}};
      Invocation context;
      std::atomic<bool> stopped{stop == 0}, done{false};
      std::atomic<std::uint64_t> polls{0};
      context.bindCancellation(&stopped);
      context.watchPolls(&polls);
      std::int64_t result = -1;
      std::jthread worker([&] {
        result = _mlir_ciface_kalandra_test_entry(&input, &context);
        done.store(true, std::memory_order_release);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (!done.load(std::memory_order_acquire) && polls.load() < stop &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      stopped.store(true, std::memory_order_release);
      worker.join();
      require(!context.status() && (result == 0 || result == 4096),
              "cancellation fault/result mismatch");
      std::sort(cells.begin(), cells.end());
      std::sort(expected.begin(), expected.end());
      require(cells == expected, "cancellation lost or duplicated elements");
      context.settle();
      require(!context.live(), "cancelled comparator sort leaked storage");
      ++cases;
    }
    std::cout << cases << " stable/stride/allocation/comparator-fault/cancellation cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
