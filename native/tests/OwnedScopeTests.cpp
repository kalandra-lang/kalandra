#include "../runtime/OwnedResources.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
using namespace kalandra::runtime;
#define CHECK(condition)                                                                            \
  do {                                                                                             \
    if (!(condition)) {                                                                             \
      std::fprintf(stderr, "owned scope assertion %d: %s\n", __LINE__, #condition);                \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
namespace {
struct Counts {
  unsigned retired = 0, destroyed = 0, depth = 0, maximumDepth = 0;
  bool recordOrder = false;
  unsigned order[3]{}, orderCount = 0;
};
struct NestedState : OwnedState {
  Counts &counts;
  unsigned id = 0;
  std::unique_ptr<Invocation> children;
  explicit NestedState(Counts &counts)
      : OwnedState(OwnedKind::ByteBuffer, sizeof(NestedState), destroyState, retireState),
        counts(counts) {}
  ~NestedState() { ++counts.destroyed; }
  static void destroyState(void *p) noexcept { static_cast<NestedState *>(p)->~NestedState(); }
  static void retireState(void *p) noexcept {
    auto &s = *static_cast<NestedState *>(p);
    CHECK(s.retired.load(std::memory_order_acquire));
    ++s.counts.retired;
    if (s.counts.recordOrder) {
      CHECK(s.counts.orderCount < 3);
      s.counts.order[s.counts.orderCount++] = s.id;
    }
    ++s.counts.depth;
    if (s.counts.depth > s.counts.maximumDepth)
      s.counts.maximumDepth = s.counts.depth;
    // This settles another logical scope and drops its only ordinary owners.
    // Retirement must pin the child and defer its cleanup until this returns.
    s.children.reset();
    --s.counts.depth;
  }
};
NestedState *make(Invocation &ctx, Counts &counts) {
  auto *memory = ctx.allocateResource(sizeof(NestedState), OwnedState::cleanup);
  CHECK(memory);
  auto *value = new (memory) NestedState(counts);
  auto *scope = currentScope(ctx);
  CHECK(scope && scope->scope);
  adopt(ctx, *value, scope->scope);
  CHECK(!ctx.poll());
  return value;
}
void nested(unsigned count, unsigned stop) {
  Counts counts;
  std::atomic<bool> cancellation{false};
  {
    Invocation root;
    root.bindCancellation(&cancellation);
    CHECK(kalandra_file_scope_enter_v1(&root, nullptr, 0));
    auto *node = make(root, counts);
    for (unsigned i = 1; i < count; ++i) {
      node->children = std::make_unique<Invocation>();
      auto &child = *node->children;
      CHECK(kalandra_file_scope_enter_v1(&child, nullptr, 0));
      node = make(child, counts);
    }
    if (stop == 1)
      root.fault(5, 12, 4);
    else if (stop == 2)
      cancellation.store(true, std::memory_order_release);
    root.settle();
    CHECK(counts.retired == count && counts.destroyed == count);
    CHECK(counts.maximumDepth == 1 && counts.depth == 0 && root.live() == 0);
    if (stop == 1)
      CHECK(root.status() == 5 && root.line() == 12 && root.column() == 4);
  }
  CHECK(counts.destroyed == count);
}
void handoff() {
  Counts counts;
  Invocation destination;
  auto *target = kalandra_file_scope_enter_v1(&destination, nullptr, 0);
  CHECK(target);
  {
    Invocation source;
    auto *scope = kalandra_file_scope_enter_v1(&source, nullptr, 0);
    CHECK(scope);
    auto *value = make(source, counts);
    auto descriptor = ownedDescriptor(value);
    CHECK(destination.retainFrom(source, value));
    const OwnedDescriptor *inputs[]{&descriptor};
    auto *borrow = kalandra_file_scope_enter_v1(&destination, inputs, 1);
    CHECK(borrow);
    kalandra_file_scope_leave_v1(&destination, borrow, &descriptor);
    kalandra_file_scope_leave_v1(&source, scope, nullptr);
    CHECK(counts.retired == 0 && counts.destroyed == 0);
  }
  CHECK(counts.retired == 0 && counts.destroyed == 0);
  kalandra_file_scope_leave_v1(&destination, target, nullptr);
  CHECK(counts.retired == 1);
  destination.settle();
  CHECK(counts.destroyed == 1 && destination.live() == 0);
}
void retirementOrder() {
  Counts counts;
  counts.recordOrder = true;
  Invocation ctx;
  auto *scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(scope);
  for (unsigned i = 0; i < 3; ++i)
    make(ctx, counts)->id = i;
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
  CHECK(counts.orderCount == 3 && counts.order[0] == 2 && counts.order[1] == 1 && counts.order[2] == 0);
  ctx.settle();
  CHECK(counts.destroyed == 3);
}
void droppedOwners() {
  Counts counts;
  Invocation ctx;
  auto *scope = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  CHECK(scope);
  for (unsigned n = 0; n < 1024; ++n) {
    auto *value = make(ctx, counts);
    ctx.release(value);
    CHECK(counts.destroyed == n + 1 && counts.retired == 0 && !scope->scope->first);
  }
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
  ctx.settle();
  CHECK(ctx.live() == 0);
}
struct LastOwnerState : OwnedState {
  Counts &counts;
  std::atomic<bool> &entered, &finish;
  LastOwnerState(Counts &counts, std::atomic<bool> &entered, std::atomic<bool> &finish)
      : OwnedState(OwnedKind::ByteBuffer, sizeof(LastOwnerState), destroyState, retireState),
        counts(counts), entered(entered), finish(finish) {}
  ~LastOwnerState() {
    entered.store(true, std::memory_order_release);
    while (!finish.load(std::memory_order_acquire))
      std::this_thread::yield();
    ++counts.destroyed;
  }
  static void destroyState(void *p) noexcept {
    static_cast<LastOwnerState *>(p)->~LastOwnerState();
  }
  static void retireState(void *p) noexcept { ++static_cast<LastOwnerState *>(p)->counts.retired; }
};
void concurrentLastOwner() {
  Counts counts;
  Invocation ctx;
  std::atomic<bool> entered{false}, finish{false};
  auto *scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(scope);
  auto *memory = ctx.allocateResource(sizeof(LastOwnerState), OwnedState::cleanup);
  CHECK(memory);
  auto *value = new (memory) LastOwnerState(counts, entered, finish);
  adopt(ctx, *value, currentScope(ctx)->scope);
  auto lease = ctx.lease(value);
  CHECK(lease);
  ctx.release(value);
  // Model a foreign I/O job releasing the final independent backing pin while
  // its former logical owner is also ending. No Invocation is shared by threads.
  std::thread job([pin = std::move(*lease)]() mutable { pin = {}; });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
  CHECK(counts.retired == 0); // A refcount-zero destructor cannot be resurrected.
  finish.store(true, std::memory_order_release);
  job.join();
  ctx.settle();
  CHECK(counts.destroyed == 1 && counts.retired == 0 && ctx.live() == 0);
}
void rejectedAdoption(bool closedResource) {
  Counts counts;
  Invocation ctx;
  auto *first = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  CHECK(first);
  auto *oldScope = first->scope;
  auto lease = ctx.lease(oldScope);
  CHECK(lease);
  auto *oldValue = make(ctx, counts);
  kalandra_file_scope_leave_v1(&ctx, first, nullptr);
  CHECK(counts.retired == 1);
  CHECK(ctx.retainLease(*lease));
  auto *second = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  CHECK(second);
  auto *newValue = make(ctx, counts);
  adopt(ctx, closedResource ? *oldValue : *newValue, closedResource ? second->scope : oldScope);
  CHECK(ctx.status() == 3 && newValue->scope == second->scope && oldValue->scope == nullptr);
  ctx.settle();
  CHECK(counts.retired == 2 && counts.destroyed == 2);
}
struct ControlProbe {
  unsigned *order;
  unsigned digit;
  static void cleanup(void *p) noexcept {
    const auto &probe = *static_cast<ControlProbe *>(p);
    *probe.order = *probe.order * 10 + probe.digit;
  }
};
void escapeRoot() {
  Counts counts;
  Invocation ctx;
  unsigned order = 0;
  auto *early = ctx.allocateControl(sizeof(ControlProbe), ControlProbe::cleanup);
  CHECK(early);
  new (early) ControlProbe{&order, 1};
  auto *outer = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  auto *inner = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  CHECK(outer && inner);
  auto *value = make(ctx, counts);
  auto descriptor = ownedDescriptor(value);
  auto *late = ctx.allocateControl(sizeof(ControlProbe), ControlProbe::cleanup);
  CHECK(late);
  new (late) ControlProbe{&order, 2};
  Invocation::OwnershipScope temporary(ctx);
  ResourceControl *root;
  {
    Invocation::OwnershipSelection selected(temporary);
    root = ensureOwnedEscapeScope(ctx);
    CHECK(root && root->scope->escapeRoot);
    CHECK(ctx.allocate(32)); // the caller's active value store was restored
  }
  CHECK(currentScope(ctx) == inner && inner->scope->parent == outer->scope);
  CHECK(outer->scope->parent == root->scope && !root->scope->parent);
  const auto allocations = ctx.allocations();
  CHECK(ensureOwnedEscapeScope(ctx) == root && ctx.allocations() == allocations);
  kalandra_file_scope_leave_v1(&ctx, inner, &descriptor);
  CHECK(order == 2 && value->scope == outer->scope && counts.retired == 0);
  kalandra_file_scope_leave_v1(&ctx, outer, &descriptor);
  CHECK(currentScope(ctx) == root && value->scope == root->scope && counts.retired == 0);
  ctx.releaseControl(early);
  CHECK(order == 21 && currentScope(ctx) == root);
  Invocation destination;
  destination.failAllocationAt(1);
  CHECK(!destination.retainFrom(ctx, value) && destination.status() == 2);
  CHECK(value->scope == root->scope && !value->retired.load());
  temporary.reset();
  CHECK(counts.retired == 0);
  ctx.settle();
  CHECK(counts.retired == 1 && counts.destroyed == 1 && ctx.live() == 0);
}
void escapeRootAdmission() {
  unsigned rejected = 0, succeeded = 0;
  for (unsigned failure = 1; failure < 14; ++failure) {
    Counts counts;
    Invocation ctx;
    ctx.failAllocationAt(failure);
    auto *scope = static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
    if (!scope) {
      CHECK(ctx.status() == 2);
      ctx.settle();
      CHECK(ctx.live() == 0);
      continue;
    }
    auto *memory = ctx.allocateResource(sizeof(NestedState), OwnedState::cleanup);
    if (!memory) {
      CHECK(ctx.status() == 2);
      ctx.settle();
      CHECK(ctx.live() == 0);
      continue;
    }
    auto *value = new (memory) NestedState(counts);
    adopt(ctx, *value, scope->scope);
    CHECK(!ctx.status());
    auto *root = ensureOwnedEscapeScope(ctx);
    if (!root) {
      ++rejected;
      CHECK(ctx.status() == 2 && currentScope(ctx) == scope && !scope->scope->parent);
      CHECK(value->scope == scope->scope && !value->retired.load() && counts.retired == 0);
    } else {
      ++succeeded;
      CHECK(!ctx.status() && scope->scope->parent == root->scope);
    }
    ctx.settle();
    CHECK(counts.retired == 1 && counts.destroyed == 1 && ctx.live() == 0);
  }
  CHECK(rejected && succeeded);
}
struct DeferredValue : OwnedState {
  Invocation body;
  BorrowedGroupOwner groups{body};
  BorrowedCleanup cleanup{this, nullptr, finish, nullptr};
  Counts &counts;
  bool finished = false;
  explicit DeferredValue(Counts &counts)
      : OwnedState(OwnedKind::Generator, sizeof(DeferredValue), destroy, finish), counts(counts) {
    deferredOwner = &groups;
    deferredCleanup = &cleanup;
  }
  static void finish(void *p) noexcept {
    auto &value = *static_cast<DeferredValue *>(p);
    if (!value.finished) {
      value.finished = true;
      value.body.settle(); // construction-only fixture: it never starts child tasks
      ++value.counts.retired;
    }
  }
  static void destroy(void *p) noexcept {
    auto &value = *static_cast<DeferredValue *>(p);
    finish(p);
    ++value.counts.destroyed;
    value.~DeferredValue();
  }
};
struct CopyBudget {
  std::uint64_t bytes = 0;
  static bool reserve(void *p, std::uint64_t count) noexcept {
    auto &budget = *static_cast<CopyBudget *>(p);
    if (count > 16 * 1024 * 1024 - budget.bytes)
      return false;
    budget.bytes += count;
    return true;
  }
  static void release(void *p, std::uint64_t count) noexcept {
    auto &budget = *static_cast<CopyBudget *>(p);
    CHECK(count <= budget.bytes);
    budget.bytes -= count;
  }
};
void ownedResultTransfer() {
  Counts counts;
  Invocation source;
  auto *root = ensureOwnedEscapeScope(source);
  CHECK(root);
  DeferredValue *value;
  {
    Invocation::OwnershipSelection selected(root->storage);
    auto *memory = source.allocateResource(sizeof(DeferredValue), OwnedState::cleanup, true);
    CHECK(memory);
    value = new (memory) DeferredValue(counts);
    CHECK(source.trackResource(value));
    adopt(source, *value, root->scope);
    value->body.bindResourceBudget(source, value);
    CHECK(value->body.allocate(64));
  }
  const auto descriptor = ownedDescriptor(value);
  {
    Invocation borrowed;
    CHECK(kalandra_owned_copy_v1(&borrowed, &source, &descriptor));
    CHECK(value->scope == root->scope && !borrowed.hasDeferredOwnedResource());
    borrowed.settle();
    CHECK(!value->finished && value->body.live() != 0);
  }
  {
    Invocation rejected;
    rejected.failAllocationAt(1);
    OwnedReceiveGuard receiving(source, rejected);
    CHECK(!kalandra_owned_copy_v1(&rejected, &source, &descriptor));
    CHECK(rejected.status() == 2 && value->scope == root->scope && !value->finished);
  }
  {
    Invocation destination, unrelated;
    OwnedReceiveGuard wrongSource(unrelated, destination);
    CHECK(kalandra_owned_copy_v1(&destination, &source, &descriptor));
    CHECK(value->scope == root->scope);
    destination.settle();
    CHECK(!value->finished);
  }
  {
    Invocation borrowed, destination;
    CHECK(kalandra_owned_copy_v1(&borrowed, &source, &descriptor));
    OwnedReceiveGuard outer(source, destination);
    CHECK(kalandra_owned_copy_v1(&destination, &borrowed, &descriptor));
    CHECK(value->scope == root->scope); // permission must match this copy's actual source
  }
  CopyBudget budget;
  Invocation destination;
  destination.bindRecursiveBudget(&budget, CopyBudget::reserve, CopyBudget::release);
  auto *scope = static_cast<ResourceControl *>(
      kalandra_file_scope_enter_v1(&destination, nullptr, 0));
  CHECK(scope);
  {
    OwnedReceiveGuard receiving(source, destination);
    // Another worker cannot inherit this thread's consuming-copy permission.
    std::thread worker([&] {
      Invocation borrowed;
      CHECK(kalandra_owned_copy_v1(&borrowed, &source, &descriptor));
      CHECK(value->scope == root->scope);
    });
    worker.join();
    CHECK(kalandra_owned_copy_v1(&destination, &source, &descriptor));
  }
  CHECK(value->scope == scope->scope && destination.hasDeferredOwnedResource());
  const auto bodyBytes = value->body.recursiveBytes();
  const auto receiverBytes = budget.bytes;
  CHECK(value->body.allocate(4096));
  CHECK(budget.bytes - receiverBytes == value->body.recursiveBytes() - bodyBytes);
  source.settle();
  CHECK(!value->finished && counts.retired == 0 && counts.destroyed == 0);
  kalandra_file_scope_leave_v1(&destination, scope, nullptr);
  CHECK(counts.retired == 1 && counts.destroyed == 1);
  destination.settle();
  CHECK(source.live() == 0 && destination.live() == 0);
  CHECK(budget.bytes == 0);
}
} // namespace
int main() {
  ownedResultTransfer();
  escapeRoot();
  escapeRootAdmission();
  for (unsigned stop = 0; stop != 3; ++stop)
    nested(12000, stop);
  handoff();
  retirementOrder();
  droppedOwners();
  concurrentLastOwner();
  rejectedAdoption(false);
  rejectedAdoption(true);
  std::puts("PASS: iterative nested owned-scope retirement, stops and handoff");
}
