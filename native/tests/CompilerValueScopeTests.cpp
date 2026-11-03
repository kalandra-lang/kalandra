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
extern "C" bool scope_test_batch(ValueScopes *, const ValueScopes::Write *, std::uint64_t);
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
  Value current{}, saved{}, partner{};
  for (unsigned round = 0; round < 10000; ++round) {
    CHECK(scope_test_begin(arena));
    const unsigned count = 1 + round % 3;
    auto *storage = ctx.allocateRecord(count * ELEMENT_BYTES);
    CHECK(storage);
    const unsigned char pattern = BOOL_ELEMENTS ? round % 2 : round % 256;
    std::memset(storage, pattern, count * ELEMENT_BYTES);
    current = {{}, {storage, storage, 0, count, 1}};
    if (round % 2) {
      partner = current;
      const ValueScopes::Write writes[] = {{0, &current}, {2, &partner}};
      CHECK(scope_test_batch(arena, writes, 2));
      CHECK(partner.data.allocated == current.data.allocated);
    } else
      CHECK(scope_test_commit(arena, 0, &current));
    if (!round) {
      saved = current;
      CHECK(scope_test_begin(arena) && scope_test_commit(arena, 1, &saved));
    }
    CHECK(static_cast<const unsigned char *>(current.data.aligned)[0] == pattern);
    CHECK(static_cast<const unsigned char *>(saved.data.aligned)[0] == 0);
    CHECK(ctx.live() == (round ? 4u : 3u) && ctx.recursiveBytes() == budget.used &&
          budget.peak < 4096);
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
  for (unsigned failure = 1; failure <= 7; ++failure) {
    Budget faultBudget;
    Invocation faultContext;
    faultContext.bindRecursiveBudget(&faultBudget, Budget::reserve, Budget::release);
    faultContext.failAllocationAt(failure);
    auto *scopes = scope_test_create(&faultContext);
    if (scopes && scope_test_begin(scopes)) {
      auto *buffer = faultContext.allocateRecord(ELEMENT_BYTES);
      if (buffer) {
        std::memset(buffer, 0, ELEMENT_BYTES);
        Value first{{}, {buffer, buffer, 0, 1, 1}}, second = first;
        const ValueScopes::Write writes[] = {{0, &first}, {2, &second}};
        CHECK(scope_test_batch(scopes, writes, 2) == (failure > 5));
      }
    }
    if (scopes)
      scope_test_destroy(scopes);
    CHECK(faultContext.status() == (failure <= 5 ? 2 : 0));
    faultContext.settle();
    CHECK(!faultContext.live() && !faultBudget.used &&
          faultContext.allocations() == faultContext.releases());
  }
  for (unsigned mode : {0u, 1u, 2u}) {
    const bool empty = mode == 1;
    Budget viewBudget;
    Invocation viewContext, reader;
    viewContext.bindRecursiveBudget(&viewBudget, Budget::reserve, Budget::release);
    auto *scopes = scope_test_create(&viewContext);
    CHECK(scopes && scope_test_begin(scopes));
    Value first{}, second{};
    if (!empty) {
      auto *buffer = viewContext.allocateRecord(7 * ELEMENT_BYTES);
      CHECK(buffer);
      std::memset(buffer, BOOL_ELEMENTS ? 1 : 0x57, 7 * ELEMENT_BYTES);
      first.data = mode == 2       ? Descriptor{buffer, buffer, 5, 3, -2}
                   : BOOL_ELEMENTS ? Descriptor{buffer, buffer, 0, 7, 1}
                                   : Descriptor{buffer, buffer, 1, 3, 2};
    }
    second = first;
    const auto original = first.data;
    const ValueScopes::Write writes[] = {{0, &first}, {2, &second}};
    const bool accepted = scope_test_batch(scopes, writes, 2);
    if (mode == 2) {
      // Negative-stride imports are outside the existing transport contract.
      CHECK(!accepted && viewContext.status() == 3);
      scope_test_destroy(scopes);
      viewContext.settle();
      CHECK(!viewContext.live() && !viewBudget.used &&
            viewContext.allocations() == viewContext.releases());
      continue;
    }
    CHECK(accepted);
    CHECK(std::memcmp(&first.data, &original, sizeof(original)) == 0);
    CHECK(std::memcmp(&second.data, &original, sizeof(original)) == 0);
    CHECK(viewContext.live() == (empty ? 1u : 3u));
    Value observed{};
    CHECK(layout->copy(reader, &observed, viewContext, &second));
    scope_test_destroy(scopes);
    viewContext.settle();
    CHECK(!viewContext.live() && !viewBudget.used &&
          viewContext.allocations() == viewContext.releases());
    if (!empty) {
      auto *bytes = static_cast<unsigned char *>(observed.data.aligned);
      CHECK(bytes[observed.data.offset * ELEMENT_BYTES] == (BOOL_ELEMENTS ? 1 : 0x57));
    }
    reader.settle();
  }
  std::puts("Actual compiler array layout: 10000 bounded transactional replacements, aliases and "
            "return publication passed");
}
