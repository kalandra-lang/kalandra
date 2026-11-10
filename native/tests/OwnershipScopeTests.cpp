#include "../runtime/Runtime.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Scope assertion %d: %s\n", __LINE__, #x);                              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
using Scope = Invocation::OwnershipScope;
using Select = Invocation::OwnershipSelection;
struct Budget {
  std::uint64_t used = 0, peak = 0, limit = 16384;
  static bool reserve(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    if (n > b.limit - b.used)
      return false;
    b.used += n;
    b.peak = std::max(b.used, b.peak);
    return true;
  }
  static void release(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    CHECK(n <= b.used);
    b.used -= n;
  }
};
static void bound(Invocation &ctx, Budget &b) {
  ctx.bindRecursiveBudget(&b, Budget::reserve, Budget::release);
}
static void repeatedReplacement() {
  Budget budget;
  Invocation ctx;
  bound(ctx, budget);
  auto *saved = ctx.allocate(64);
  CHECK(saved);
  std::memset(saved, 0x17, 64);
  const auto rootCharge = ctx.recursiveBytes();
  Scope work(ctx), slot(ctx), spare(ctx);
  CHECK(work.valid() && slot.valid() && spare.valid());
  void *current = nullptr;
  std::uint64_t peak = 0;
  for (unsigned i = 0; i < 10000; ++i) {
    void *next;
    {
      Select selection(work);
      next = ctx.allocate(32 + i % 3 * 32);
      CHECK(next);
      std::memset(next, i % 256, 32 + i % 3 * 32);
    }
    {
      Select selection(spare);
      CHECK(ctx.retainFrom(ctx, next));
    }
    peak = std::max(peak, ctx.live());
    slot.reset();
    work.reset();
    CHECK(slot.swap(spare));
    current = next;
    auto lease = ctx.lease(current);
    CHECK(lease && lease->bytes()[0] == std::byte(i % 256));
    CHECK(ctx.live() == 2 && ctx.recursiveBytes() == budget.used);
    auto old = ctx.lease(saved);
    CHECK(old && old->bytes()[0] == std::byte{0x17});
  }
  CHECK(peak == 4 && budget.peak < 4096);
  slot.reset();
  CHECK(ctx.recursiveBytes() == rootCharge);
  CHECK(ctx.live() == 1);
  ctx.settle();
  CHECK(!budget.used && !ctx.live() && ctx.allocations() == ctx.releases());
}
static void graphPublication() {
  static int key;
  Budget budget;
  Invocation ctx;
  bound(ctx, budget);
  Scope work(ctx), slot(ctx), spare(ctx);
  const void *current = nullptr;
  for (unsigned round = 0; round < 5000; ++round) {
    const void *next;
    {
      Select select(work);
      auto *leaf = ctx.allocate(32);
      CHECK(leaf);
      std::memset(leaf, round % 256, 32);
      auto lease = ctx.lease(leaf);
      CHECK(lease);
      const std::array<std::byte, 8> payload{std::byte(round % 256)};
      next = ctx.makeRecursiveValue(payload, &key, std::span<const AllocationLease>(&*lease, 1));
      CHECK(next);
    }
    {
      Select select(spare);
      auto lease = ctx.lease(next);
      CHECK(lease && ctx.retainRecursiveGraph(*lease, &key, 8, 1));
    }
    slot.reset();
    work.reset();
    CHECK(slot.swap(spare));
    current = next;
    CHECK(ctx.live() == 2 && budget.used == ctx.recursiveBytes() && budget.used > 0);
    auto root = ctx.lease(current);
    CHECK(root);
    auto child = root->dependency(0);
    CHECK(child && child->bytes()[0] == std::byte(round % 256));
  }
  auto keep = ctx.lease(current);
  CHECK(keep);
  slot.reset();
  CHECK(!budget.used && !ctx.live());
  auto leaf = keep->dependency(0);
  CHECK(leaf && leaf->bytes()[0] == std::byte(4999 % 256));
  ctx.settle();
  CHECK(ctx.allocations() == ctx.releases());
}
static void faultAttempts() {
  for (unsigned fail = 1; fail <= 10; ++fail) {
    Budget b;
    Invocation ctx;
    bound(ctx, b);
    ctx.failAllocationAt(fail);
    Scope work(ctx), slot(ctx);
    unsigned completed = 0;
    for (unsigned i = 0; i < 10; ++i) {
      void *p;
      {
        Select select(work);
        p = ctx.allocate(64);
      }
      if (!p)
        break;
      ++completed;
      work.reset();
      slot.reset();
    }
    CHECK(completed == fail - 1 && ctx.status() == 2);
    work.reset();
    slot.reset();
    CHECK(!b.used && !ctx.live());
    CHECK(ctx.poll() == 2);
    ctx.settle();
    CHECK(ctx.allocations() == ctx.releases());
  }
}
static void cancellationAndPrecedence() {
  for (bool priorFault : {false, true}) {
    std::atomic<bool> cancel = false;
    std::atomic<std::uint64_t> polls = 0;
    Budget b;
    Invocation ctx;
    bound(ctx, b);
    ctx.bindCancellation(&cancel);
    ctx.watchPolls(&polls);
    Scope work(ctx);
    {
      Select select(work);
      CHECK(ctx.allocate(64));
    }
    if (priorFault)
      ctx.fault(5, 7, 9);
    cancel = true;
    {
      Select select(work);
      CHECK(!ctx.allocate(64));
    }
    CHECK(ctx.poll() == (priorFault ? 5 : -1));
    CHECK(ctx.status() == (priorFault ? 5 : 0));
    work.reset();
    CHECK(!ctx.live() && !b.used && polls > 0);
    ctx.settle();
  }
}
static void environmentsAndRootSettlement() {
  Budget b;
  Invocation ctx;
  ctx.bindEnvironmentBudget(&b, Budget::reserve, Budget::release);
  Scope a(ctx), bScope(ctx);
  static int key;
  for (unsigned i = 0; i < 10000; ++i) {
    {
      Select select(a);
      CHECK(ctx.allocateTransportStorage(64, &key));
    }
    CHECK(ctx.environmentBytes() == 64 && b.used == 64);
    a.reset();
    CHECK(!b.used && !ctx.environmentBytes());
  }
  {
    Select select(a);
    CHECK(ctx.allocateTransportStorage(64, &key));
  }
  {
    Select select(bScope);
    CHECK(ctx.allocateTransportStorage(64, &key));
  }
  ctx.settle();
  CHECK(!b.used && !ctx.live() && !ctx.environmentBytes());
  a.reset();
  bScope.reset();
  CHECK(ctx.allocations() == ctx.releases());
}
static void migrationAndForeignRetention() {
  Invocation ctx, dest;
  Scope work(ctx), slot(ctx);
  void *p;
  {
    Select select(work);
    p = ctx.allocate(64);
    CHECK(p);
    std::memset(p, 0x35, 64);
  }
  std::thread worker([&] {
    Select select(slot);
    CHECK(ctx.retainFrom(ctx, p));
  });
  worker.join();
  work.reset();
  CHECK(dest.retainFrom(ctx, p));
  slot.reset();
  ctx.settle();
  auto retained = dest.lease(p);
  CHECK(retained && retained->bytes()[0] == std::byte{0x35});
  dest.settle();
}
static void transactionalFailure() {
  for (unsigned fail = 2; fail <= 6; ++fail) {
    Budget b;
    Invocation ctx;
    bound(ctx, b);
    ctx.failAllocationAt(fail);
    Scope work(ctx), slot(ctx), spare(ctx);
    void *old;
    {
      Select select(slot);
      old = ctx.allocate(64);
      CHECK(old);
      std::memset(old, 0x29, 64);
    }
    {
      Select select(work);
      auto *fresh = ctx.allocate(64);
      if (fresh) {
        Select target(spare);
        ctx.retainFrom(ctx, fresh);
      }
    }
    // Retain-before-retire: the old slot remains alive until publication succeeds.
    CHECK(ctx.live() >= 1);
    slot.reset();
    work.reset();
    spare.reset();
    CHECK(!ctx.live() && !b.used);
    ctx.settle();
    CHECK(ctx.allocations() == ctx.releases());
  }
}

static void graphRollback() {
  static int key;
  for (unsigned fail = 2; fail <= 12; ++fail) {
    Budget budget;
    Invocation ctx;
    bound(ctx, budget);
    ctx.failAllocationAt(fail);
    auto *saved = ctx.allocate(64);
    CHECK(saved);
    std::memset(saved, 0x61, 64);
    Scope work(ctx), spare(ctx);
    {
      Select select(work);
      auto *leaf = ctx.allocate(32);
      if (leaf) {
        auto dependency = ctx.lease(leaf);
        CHECK(dependency);
        const std::array<std::byte, 8> payload{};
        auto *root = ctx.makeRecursiveValue(payload, &key,
                                            std::span<const AllocationLease>(&*dependency, 1));
        if (root) {
          auto retained = ctx.lease(root);
          CHECK(retained);
          Select destination(spare);
          ctx.retainRecursiveGraph(*retained, &key, 8, 1);
        }
      }
    }
    CHECK(static_cast<unsigned char *>(saved)[0] == 0x61);
    work.reset();
    spare.reset();
    CHECK(ctx.live() == 1 && ctx.recursiveBytes() == budget.used);
    ctx.settle();
    CHECK(!ctx.live() && !budget.used && ctx.allocations() == ctx.releases());
  }
}
static void expectedErrors() {
  for (bool application : {false, true}) {
    Invocation ctx;
    Scope scope(ctx);
    {
      Select select(scope);
      CHECK(ctx.allocate(64));
    }
    if (application)
      ctx.expectedApplicationError("Example", "Failure");
    else
      ctx.expectedTaskError(2, 0, 0, 0);
    {
      Select select(scope);
      CHECK(!ctx.allocate(64));
    }
    scope.reset();
    CHECK(ctx.hasExpectedError() && !ctx.status() && !ctx.live());
    ctx.settle();
    CHECK(ctx.hasExpectedError() && !ctx.status());
  }
}
struct ResourceProbe {
  std::atomic<unsigned> *closed;
  static void cleanup(void *p) noexcept {
    auto *probe = static_cast<ResourceProbe *>(p);
    probe->closed->fetch_add(1, std::memory_order_relaxed);
    probe->~ResourceProbe();
  }
};
static void scopedResources() {
  std::atomic<unsigned> closed{0};
  for (unsigned mode = 0; mode < 4; ++mode) {
    Invocation source, destination;
    Scope local(source);
    void *resource;
    {
      Select select(local);
      resource = source.allocateResource(sizeof(ResourceProbe), ResourceProbe::cleanup);
      CHECK(resource);
      new (resource) ResourceProbe{&closed};
    }
    auto lease = source.lease(resource);
    CHECK(lease);
    CHECK(destination.retainFrom(source, resource));
    local.reset();
    CHECK(closed == mode);
    if (mode == 1)
      destination.fault(1, 12, 3);
    else if (mode == 2)
      destination.expectedTaskError(2, 0, 0, 0);
    else if (mode == 3) {
      static std::atomic<bool> cancelled{true};
      destination.bindCancellation(&cancelled);
      CHECK(destination.poll() == -1);
    }
    source.settle();
    destination.settle();
    CHECK(!source.live() && !destination.live() && closed == mode);
    // The final lease may retire on a different worker; cleanup is exactly once.
    std::thread worker([keep = std::move(*lease)] {});
    worker.join();
    CHECK(closed == mode + 1);
  }
  // Failed allocation never publishes a resource or calls its cleanup.
  Invocation failed;
  failed.failAllocationAt(1);
  CHECK(!failed.allocateResource(sizeof(ResourceProbe), ResourceProbe::cleanup));
  failed.settle();
  CHECK(closed == 4 && failed.status() == 2 && !failed.live());
  // Repeated scoped resources cannot leak into the small numeric-backing cache.
  Invocation repeated;
  for (unsigned n = 0; n < 10000; ++n) {
    Scope local(repeated);
    {
      Select select(local);
      auto *p = repeated.allocateResource(sizeof(ResourceProbe), ResourceProbe::cleanup);
      CHECK(p);
      new (p) ResourceProbe{&closed};
    }
    local.reset();
    CHECK(closed == n + 5 && !repeated.live());
  }
  repeated.settle();
}
int main() {
  scopedResources();
  repeatedReplacement();
  graphPublication();
  faultAttempts();
  cancellationAndPrecedence();
  environmentsAndRootSettlement();
  migrationAndForeignRetention();
  transactionalFailure();
  graphRollback();
  expectedErrors();
  std::puts("Scoped owners: bounded replacement, graphs, shared quotas/failure lineage, "
            "cancellation, migration and cleanup passed");
}
