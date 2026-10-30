#include "../runtime/NativeCancellation.h"
#include "../runtime/Runtime.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
struct Descriptor { void *allocated,*data; std::int64_t offset,size,stride; };
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(const Descriptor *,bool,Invocation *);
int main() {
  const bool available = initializeNativeCancellation();
  std::int64_t input = 42;
  const Descriptor descriptor{&input,&input,0,1,1};
  {
    Invocation context;
    if (_mlir_ciface_kalandra_test_entry(&descriptor,true,&context) != 42 || context.status())
      throw std::runtime_error("array spin terminal branch changed");
  }
  for (bool native : {false,true}) {
    Invocation context;
    std::atomic<bool> token{false}, finished{false};
    context.bindCancellation(&token);
    const auto thread = available ? nativeCancellationThread() : 0;
    const bool redirect = native && thread && nativeCancellationCanAbandon();
    context.allowNativeInterrupts(redirect);
    const auto before = nativeCancellationRedirects();
    std::jthread stop([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      token.store(true,std::memory_order_release);
      const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(3);
      while (!finished.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > until) std::abort();
        if (redirect) interruptNativeCancellation(thread);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
    const auto result = _mlir_ciface_kalandra_test_entry(&descriptor,false,&context);
    const bool cancelled = token.load(std::memory_order_acquire);
    finished.store(true,std::memory_order_release);
    stop.join();
    if (!cancelled || result || context.status() || context.callDepth() || input != 42 ||
        (redirect && nativeCancellationRedirects() == before))
      throw std::runtime_error("array spin lost divergence/cancellation/redirect semantics");
    context.settle();
    if (context.live() || context.allocations() != context.releases())
      throw std::runtime_error("cancelled array spin leaked backing");
  }
  std::cout << "Array spin: divergent source stays divergent until cancellation; cooperative "
               "and actual native redirects (where supported) settle borrowed inputs safely\n";
}
