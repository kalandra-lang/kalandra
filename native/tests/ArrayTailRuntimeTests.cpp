#include "../runtime/NativeCancellation.h"
#include "../runtime/Runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
static std::uint64_t peak;
extern "C" void *test_alloc(Invocation *context, std::uint64_t bytes) noexcept {
  auto *value = kalandra_alloc_v1(context,bytes);
  peak = std::max(peak,context->live());
  return value;
}
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t,std::int64_t,Invocation *);
static void check(bool ok) {
  if (!ok) throw std::runtime_error("array tail lifetime/numerical regression");
}
int main() {
  const bool interrupts = initializeNativeCancellation();
  for (int mode = 0; mode != 4; ++mode)
    for (std::int64_t seed : {1,17,31})
      for (std::int64_t rounds : {0,1,16,256,100000}) {
        Invocation context;
        context.failAllocationAt(0);
        std::atomic<bool> token{false};
        std::atomic<std::uint64_t> polls{0};
        if (mode) context.bindCancellation(&token);
        if (mode == 2) context.watchPolls(&polls);
        if (mode == 3) context.allowNativeInterrupts(interrupts);
        std::array<std::int64_t,8> values;
        for (unsigned i = 0; i < values.size(); ++i) values[i] = seed+i;
        for (std::int64_t n = 0; n < rounds; ++n)
          for (auto &value : values) value = (value*13+7)%1000003;
        std::int64_t expected = 0;
        for (auto value : values) expected += value;
        peak = 0;
        check(_mlir_ciface_kalandra_test_entry(seed,rounds,&context) == expected);
        check(!context.status() && !context.callDepth() && !context.live());
        // Source tail recursion must not retain one backing/native frame per
        // step merely because deallocation was inserted after a recursive call.
        check(peak <= 4);
        if (mode == 2) check(polls.load() != 0);
        context.settle();
        check(!context.live() && context.allocations() == context.releases());
      }
  std::uint64_t allocations;
  {
    Invocation context;
    context.failAllocationAt(0);
    check(_mlir_ciface_kalandra_test_entry(17,16,&context) > 0 && !context.status());
    allocations = context.allocations();
  }
  for (std::uint64_t attempt = 1; attempt <= allocations+1; ++attempt) {
    Invocation context;
    context.failAllocationAt(attempt);
    const auto result = _mlir_ciface_kalandra_test_entry(17,16,&context);
    check(attempt <= allocations ? result == 0 && context.status() == 2
                                : result > 0 && !context.status());
    context.settle();
    check(!context.live() && !context.callDepth() && context.allocations() == context.releases());
  }
  for (bool native : {false,true}) {
    Invocation context;
    std::atomic<bool> token{false};
    context.bindCancellation(&token);
    context.allowNativeInterrupts(native && interrupts);
    std::jthread stop([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      token.store(true,std::memory_order_release);
    });
    const auto start = std::chrono::steady_clock::now();
    check(_mlir_ciface_kalandra_test_entry(17,1000000000000LL,&context) == 0);
    stop.join();
    check(!context.status() && !context.callDepth() &&
          std::chrono::steady_clock::now()-start < std::chrono::seconds(5));
    context.settle();
    check(!context.live() && context.allocations() == context.releases());
  }
  std::cout << "Array tail lifetimes: 60 exact oracle/mode cases through 100,000 steps, "
               "every allocation fault, cooperative/native cancellation, "
               "at most four live backings, observed polls and balanced cleanup passed\n";
}
