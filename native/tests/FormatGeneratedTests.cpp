#include "../runtime/String.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" void _mlir_ciface_kalandra_test_entry(StringDescriptor *, std::int64_t, Invocation *);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void settle(Invocation &ctx) {
  ctx.settle();
  require(!ctx.live() && ctx.allocations() == ctx.releases(), "generated format leaked");
}
int main() {
  try {
    // There are no owned arguments: checked division must happen before the
    // descriptor table, scalar rendering or any template allocation.
    {
      Invocation ctx;
      ctx.failAllocationAt(1);
      StringDescriptor out;
      _mlir_ciface_kalandra_test_entry(&out, 0, &ctx);
      require(ctx.status() == 4 && !out.data && !ctx.allocations(),
              "format allocation incorrectly won over argument arithmetic");
      settle(ctx);
    }
    unsigned successes = 0, failures = 0;
    for (unsigned ordinal = 0; ordinal < 48; ++ordinal) {
      Invocation ctx;
      if (ordinal)
        ctx.failAllocationAt(ordinal);
      StringDescriptor out;
      _mlir_ciface_kalandra_test_entry(&out, 2, &ctx);
      if (ctx.status()) {
        ++failures;
        require(ctx.status() == 2 && !out.data && !out.byteLength,
                "failed format published a value");
      } else {
        ++successes;
        require(validateString(ctx, out) && out.byteLength == 4 &&
                    !std::memcmp(out.data, "1 50", 4),
                "generated format text changed");
      }
      settle(ctx);
    }
    require(successes && failures, "allocation sweep failed to exercise both outcomes");
    std::atomic<bool> stop{true};
    for (bool faulted : {false, true}) {
      Invocation ctx;
      StringDescriptor out;
      ctx.bindCancellation(&stop);
      if (faulted)
        ctx.fault(7);
      _mlir_ciface_kalandra_test_entry(&out, 0, &ctx);
      require(ctx.status() == (faulted ? 7 : 0) && !out.data && !ctx.allocations(),
              "generated format changed cancellation/fault precedence");
      settle(ctx);
    }
    std::cout << "PASS: source argument fault precedence, exact text, allocation sweep and "
                 "cancellation cleanup\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
