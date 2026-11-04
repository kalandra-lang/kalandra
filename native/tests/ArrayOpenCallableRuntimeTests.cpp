#include "../runtime/Runtime.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" std::int64_t kalandra_host_first(std::int64_t, Invocation *);
extern "C" std::int64_t kalandra_host_second(std::int64_t, Invocation *);
int main() {
  try {
    // Main passes only first() to apply(). A private host entry can supply
    // second(), so main's inferred target must not be treated as exhaustive.
    for (auto n : {-41LL, 1LL, 40LL, 120LL})
      for (bool second : {false, true}) {
        Invocation context;
        auto value = second ? kalandra_host_second(n, &context)
                            : kalandra_host_first(n, &context);
        if (context.status() || value != n + (second ? 2 : 1))
          throw std::runtime_error("open callable input was incorrectly specialized to main's target");
        context.settle();
        if (context.live() || context.allocations() != context.releases())
          throw std::runtime_error("open callable result leaked ownership");
      }
    {
      Invocation context;
      std::atomic<bool> stopped{true};
      context.bindCancellation(&stopped);
      if (kalandra_host_second(40, &context) || context.status() || context.allocations())
        throw std::runtime_error("open callable bypassed entry cancellation");
      context.settle();
    }
    std::cout << "Open private callable input: both valid targets, exact results, cancellation and cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
