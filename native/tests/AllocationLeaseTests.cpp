#include "../runtime/Runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Allocation lease assertion %d\n", __LINE__);                           \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  for (unsigned bytes : {0u, 1u, 64u, 128u, 256u, 257u}) {
    Invocation context;
    context.failAllocationAt(0);
    for (unsigned round = 0; round < 1000; ++round) {
      auto *value = context.allocate(bytes);
      CHECK(value && context.live() == 1);
      std::memset(value, 0x59, bytes);
      context.release(value);
      CHECK(!context.live() && !context.lease(value));
    }
    CHECK(context.allocations() == 1000 && context.releases() == 1000);
    context.settle();
  }
  {
    Invocation context;
    auto *numeric = context.allocate(64);
    CHECK(numeric);
    context.release(numeric);
    auto *record = context.allocateRecord(64);
    CHECK(record == numeric);
    std::memset(record, 0x39, 64);
    auto lease = context.lease(record);
    CHECK(lease);
    context.settle();
    for (auto byte : lease->bytes()) CHECK(byte == std::byte{0x39});
    CHECK(context.allocations() == context.releases());
  }
  {
    Invocation context;
    context.failAllocationAt(2);
    auto *value = context.allocate(64);
    CHECK(value);
    context.release(value);
    CHECK(!context.allocate(64) && context.status() == 2);
    context.settle();
    CHECK(context.allocations() == 1 && context.releases() == 1);
  }
  {
    struct Budget {
      std::uint64_t used = 0, limit = UINT64_MAX;
      static bool reserve(void *state, std::uint64_t bytes) noexcept {
        auto &budget = *static_cast<Budget *>(state);
        if (bytes > budget.limit - budget.used) return false;
        budget.used += bytes;
        return true;
      }
      static void release(void *state, std::uint64_t bytes) noexcept {
        static_cast<Budget *>(state)->used -= bytes;
      }
    } budget;
    Invocation context;
    context.failAllocationAt(0);
    context.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    auto *value = context.allocate(64);
    CHECK(value && budget.used);
    budget.limit = budget.used * 3;
    context.release(value);
    for (unsigned n = 0; n < 2; ++n) {
      value = context.allocate(64);
      CHECK(value);
      context.release(value);
    }
    CHECK(budget.used == budget.limit && !context.allocate(64) && context.status() == 2);
    context.settle();
    CHECK(!budget.used && context.allocations() == context.releases());
  }
  {
    Invocation source, destination;
    auto *original = source.allocate(64);
    CHECK(original);
    std::memset(original, 0x27, 64);
    auto held = source.lease(original);
    CHECK(held && destination.retainFrom(source, original));
    source.release(original);
    for (unsigned n = 0; n < 100; ++n) {
      auto *replacement = source.allocate(64);
      CHECK(replacement && replacement != original);
      std::memset(replacement, 0x71, 64);
      source.release(replacement);
    }
    source.settle();
    destination.settle();
    for (auto byte : held->bytes()) CHECK(byte == std::byte{0x27});
  }
  for (unsigned n : {0u, 1u, 1024u}) {
    Invocation source;
    auto *p = source.allocate(n);
    CHECK(p);
    if (n)
      std::memset(p, 0x5a, n);
    auto lease = source.lease(p);
    CHECK(lease && bool(*lease) && lease->bytes().size() == n);
    CHECK(lease->bytes().data() == p);
    AllocationLease moved = std::move(*lease);
    CHECK(!*lease);
    AllocationLease assigned;
    assigned = std::move(moved);
    CHECK(!moved);
    source.release(p);
    CHECK(!source.live());
    CHECK(!source.lease(p));
    source.settle();
    for (auto b : assigned.bytes())
      CHECK(b == std::byte{0x5a});
  }
  {
    Invocation a, b;
    auto *p = a.allocate(64);
    CHECK(p);
    std::memset(p, 0x2a, 64);
    CHECK(!b.lease(p));
    CHECK(!a.lease(nullptr));
    CHECK(!a.lease(reinterpret_cast<void *>(0x1000)));
    CHECK(b.retainFrom(a, p));
    auto lease = b.lease(p);
    CHECK(lease);
    a.settle();
    b.settle();
    CHECK(lease->bytes().size() == 64 && lease->bytes()[63] == std::byte{0x2a});
  }
  {
    Invocation a;
    auto *p = a.allocate(4);
    CHECK(p);
    auto lease = a.lease(p);
    CHECK(lease);
    a.fault(1);
    CHECK(!a.lease(p));
    CHECK(lease->bytes().size() == 4);
    a.settle();
    CHECK(!a.lease(p));
  }
  {
    Invocation a;
    auto *p = a.allocate(1);
    CHECK(p);
    a.expectedTaskError(1, 2, 0, 0);
    CHECK(!a.lease(p));
  }
  // The final invocation owner and a retained reader may retire on different threads.
  for (unsigned bytes : {64u, 4096u})
  for (unsigned i = 0; i < 128; ++i) {
    Invocation a;
    auto *p = a.allocate(bytes);
    CHECK(p);
    std::memset(p, 0x73, bytes);
    auto lease = a.lease(p);
    CHECK(lease);
    std::atomic<bool> ready{false};
    std::thread reader([value = std::move(*lease), &ready] {
      ready = true;
      for (auto b : value.bytes())
        CHECK(b == std::byte{0x73});
    });
    while (!ready.load())
      std::this_thread::yield();
    a.settle();
    reader.join();
    CHECK(!a.live());
  }
  std::puts("Allocation leases preserve immutable bytes across invocation settlement and "
            "concurrent final release");
}
