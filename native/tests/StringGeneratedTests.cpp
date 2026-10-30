#include "../runtime/String.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" void _mlir_ciface_kalandra_string_entry(StringDescriptor *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
#ifndef STRING_FAILURE_POSITIONS
#define STRING_FAILURE_POSITIONS 8
#endif
int main() {
  try {
    const char expected[] = "سلام\0👩‍💻";
    for (unsigned attempt = 0; attempt < STRING_FAILURE_POSITIONS; ++attempt) {
      Invocation ctx;
      if (attempt)
        ctx.failAllocationAt(attempt);
      StringDescriptor value;
      _mlir_ciface_kalandra_string_entry(&value, &ctx);
      if (!ctx.status()) {
        require(validateString(ctx, value), "generated String invalid");
        require(value.byteLength == sizeof(expected) - 1 &&
                    !std::memcmp(value.data, expected, sizeof(expected) - 1),
                "generated String bytes changed");
        auto keep = ctx.lease(value.allocated);
        require(bool(keep), "generated String not owned");
        Invocation stranger;
        require(!validateString(stranger, value), "foreign invocation descriptor accepted");
        ctx.settle();
        require(!std::memcmp(keep->bytes().data(), expected, sizeof(expected) - 1),
                "generated helper result did not survive settle");
      } else
        require(!value.data && !value.byteLength, "failed entry published String");
      ctx.settle();
      require(!ctx.live() && ctx.allocations() == ctx.releases(), "generated String leaked");
    }
    std::cout << "generated helpers preserve UTF-8, owned lifetimes, invocation isolation and "
                 "failure cleanup\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
