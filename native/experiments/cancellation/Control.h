#pragma once
#include "../../runtime/Runtime.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <setjmp.h>

// Experimental ABI, deliberately separate from the language/runtime ABI.
// A reserved callee-saved register carries this pointer through generated code.
namespace kalandra::experiment {
struct alignas(64) Control {
  std::atomic<std::uint32_t> requested{0};
  std::uint32_t reserved = 0;
  void *page = nullptr;
  std::uint64_t credit = 256;
  sigjmp_buf landing;
  std::atomic<std::uint32_t> active{0}, acknowledged{0}, done{0};
  std::atomic<bool> token{false};
  runtime::Invocation *invocation = nullptr;
  std::uint64_t initialDepth = 0;
  double result = 0;
  std::int64_t seed = 17, depth = 14;
  std::uint64_t failAt = 0;
  std::int64_t expectedStatus = 0;
  std::atomic<std::uint64_t> stackLimit{0};
  std::atomic<std::int64_t> finishedNs{0};
};
struct Region {
  const void *begin, *end;
};
static_assert(offsetof(Control, requested) == 0 && offsetof(Control, page) == 8 &&
              offsetof(Control, credit) == 16);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
} // namespace kalandra::experiment
extern "C" {
extern const kalandra::experiment::Region kalandra_cancel_regions[];
extern const std::uint64_t kalandra_cancel_region_count;
void kalandra_cancel_enter(void (*)(void *), void *, kalandra::experiment::Control *);
[[noreturn]] void kalandra_cancel_jump();
[[noreturn]] void kalandra_cancel_unwind();
}
