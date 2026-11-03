#include "../runtime/ValueScopes.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Compiler scope assertion %d: %s\n", __LINE__, #x);                     \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
extern "C" const ValueLayout *scope_test_layout();
extern "C" ValueScopes *scope_test_create(Invocation *);
extern "C" bool scope_test_begin(ValueScopes *);
extern "C" bool scope_test_commit(ValueScopes *, std::uint64_t, void *);
extern "C" void scope_test_destroy(ValueScopes *);
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset, size, stride;
};
struct Value {
  Failure failure;
  Descriptor data;
};
static_assert(sizeof(Value) == 72);
struct Budget {
  std::uint64_t used = 0, peak = 0;
  static bool reserve(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    if (n > 16384 - b.used)
      return false;
    b.used += n;
    b.peak = std::max(b.peak, b.used);
    return true;
  }
  static void release(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    CHECK(n <= b.used);
    b.used -= n;
  }
};
int main() {
  Budget budget;
  Invocation ctx;
  ctx.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
  auto *layout = scope_test_layout();
  CHECK(layout && layout->bytes == sizeof(Value) && layout->alignment == alignof(Value));
  auto *arena = scope_test_create(&ctx);
  CHECK(arena);
  Value current{}, saved{};
  for (unsigned round = 0; round < 10000; ++round) {
    CHECK(scope_test_begin(arena));
    const unsigned count = 1 + round % 3;
    auto *storage = ctx.allocateRecord(count * ELEMENT_BYTES);
    CHECK(storage);
    const unsigned char pattern = BOOL_ELEMENTS ? round % 2 : round % 256;
    std::memset(storage, pattern, count * ELEMENT_BYTES);
    current = {{}, {storage, storage, 0, count, 1}};
    CHECK(scope_test_commit(arena, 0, &current));
    if (!round) {
      saved = current;
      CHECK(scope_test_begin(arena) && scope_test_commit(arena, 1, &saved));
    }
    CHECK(static_cast<const unsigned char *>(current.data.aligned)[0] == pattern);
    CHECK(static_cast<const unsigned char *>(saved.data.aligned)[0] == 0);
    CHECK(ctx.live() == 3 && ctx.recursiveBytes() == budget.used && budget.peak < 4096);
  }
  Invocation observer;
  Value output{};
  CHECK(layout->copy(observer, &output, ctx, &current));
  scope_test_destroy(arena);
  ctx.settle();
  CHECK(!ctx.live() && !budget.used && ctx.allocations() == ctx.releases());
  CHECK(static_cast<const unsigned char *>(output.data.aligned)[0] ==
        (BOOL_ELEMENTS ? 9999 % 2 : 9999 % 256));
  observer.settle();
  for (unsigned failure = 1; failure <= 6; ++failure) {
    Budget faultBudget;
    Invocation faultContext;
    faultContext.bindRecursiveBudget(&faultBudget, Budget::reserve, Budget::release);
    faultContext.failAllocationAt(failure);
    auto *scopes = scope_test_create(&faultContext);
    if (scopes) {
      Value input{};
      for (unsigned round = 0; round < 2; ++round) {
        if (!scope_test_begin(scopes))
          break;
        auto *buffer = faultContext.allocateRecord(ELEMENT_BYTES);
        if (!buffer)
          break;
        std::memset(buffer, 0, ELEMENT_BYTES);
        input = {{}, {buffer, buffer, 0, 1, 1}};
        if (!scope_test_commit(scopes, 0, &input))
          break;
      }
      scope_test_destroy(scopes);
    }
    CHECK(faultContext.status() == (failure <= 5 ? 2 : 0));
    faultContext.settle();
    CHECK(!faultContext.live() && !faultBudget.used &&
          faultContext.allocations() == faultContext.releases());
  }
  std::puts("Actual compiler array layout: 10000 bounded transactional replacements, aliases and "
            "return publication passed");
}
