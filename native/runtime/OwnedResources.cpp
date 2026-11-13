#include "OwnedResources.h"
#include <utility>
namespace kalandra::runtime {
namespace {
// Nested owned scopes can retire without nesting C++ cleanup frames. No source
// code or scheduler work runs here, and this is not a current Invocation.
struct RetirementQueue {
  OwnedState *first = nullptr, *last = nullptr;
  bool draining = false;
  void append(OwnedState &value) noexcept {
    value.retirementNext = nullptr;
    if (last)
      last->retirementNext = &value;
    else
      first = &value;
    last = &value;
  }
  void drain() noexcept {
    if (draining)
      return;
    draining = true;
    while (first) {
      auto *value = first;
      first = std::exchange(value->retirementNext, nullptr);
      if (!first)
        last = nullptr;
      auto pin = std::move(value->scopePin);
      auto scopeOwner = std::move(value->scopeOwner);
      {
        std::unique_lock access(value->mutex, std::try_to_lock);
        if (access)
          value->retire(value);
      }
      // In-flight foreign I/O may hold the state mutex. Its own backing pin
      // keeps the retired state alive until the operation ends. Both pins
      // release after the state lock, outside the registry lock.
    }
    draining = false;
  }
};
thread_local RetirementQueue retirements;
} // namespace
std::mutex &resourceMutex() {
  static auto *value = new std::mutex;
  return *value;
}
void OwnedState::detach() noexcept {
  if (!scope)
    return;
  if (previous)
    previous->next = next;
  else
    scope->first = next;
  if (next)
    next->previous = previous;
  scope = nullptr;
  previous = next = nullptr;
}
OwnedState::~OwnedState() {
  std::lock_guard lock(resourceMutex());
  detach();
}
ResourceControl::~ResourceControl() {
  if (!scope)
    return;
  {
    std::lock_guard lock(resourceMutex());
    scope->active = false;
    while (scope->first) {
      auto *value = scope->first;
      auto pin = value->pinForRetirement();
      value->detach();
      value->retired.store(true, std::memory_order_release);
      if (pin) {
        value->scopePin = std::move(*pin);
        retirements.append(*value);
      }
      // Without a pin, the last owner is already destroying the object. Its
      // destructor finishes cleanup after the registry lock is released.
    }
  }
  retirements.drain();
}
ResourceControl *currentScope(Invocation &ctx) noexcept {
  return static_cast<ResourceControl *>(ctx.latestControl(ResourceControl::cleanup));
}
void adopt(Invocation &ctx, OwnedState &file, ResourceScope *scope) noexcept {
  auto owner = scope ? ctx.lease(scope) : std::optional<AllocationLease>{};
  auto pin = scope ? ctx.lease(&file) : std::optional<AllocationLease>{};
  if (scope && (!owner || !pin)) {
    ctx.fault(3);
    return;
  }
  // An asynchronous producer lives to the end of its logical scope. Physical
  // references held by children cannot replace that scope's budget reservation.
  // Only these producers receive the extra scope owner; ordinary resources keep
  // their existing final-value retirement behavior.
  if (scope && file.deferredOwner) {
    {
      std::lock_guard lock(resourceMutex());
      if (file.scope == scope)
        return;
      if (!scope->active || scope->retiring || file.retired.load(std::memory_order_acquire) ||
          scope->context != &ctx || !scope->control) {
        ctx.fault(3);
        return;
      }
    }
    Invocation::OwnershipSelection selected(scope->control->storage);
    if (!ctx.retainLease(*pin))
      return;
    scope->hasDeferred.store(true, std::memory_order_release);
    ctx.markDeferredOwnedResource();
  }
  AllocationLease previousOwner;
  {
    std::lock_guard lock(resourceMutex());
    if (file.retired.load(std::memory_order_acquire) ||
        (scope && (!scope->active || scope->retiring))) {
      ctx.fault(3);
      return;
    }
    file.detach();
    previousOwner = std::move(file.scopeOwner);
    file.scopeOwner = owner ? std::move(*owner) : AllocationLease{};
    if (scope) {
      file.scope = scope;
      file.next = scope->first;
      if (file.next)
        file.next->previous = &file;
      scope->first = &file;
    }
  }
}
OwnedDescriptor ownedDescriptor(OwnedState *value) noexcept {
  OwnedDescriptor result;
  if (value) {
    auto &s = result.storage;
    s.allocated = s.aligned = reinterpret_cast<std::uint8_t *>(value);
    s.sizes[0] = static_cast<std::int64_t>(value->bytes);
    s.strides[0] = 1;
  }
  return result;
}
OwnedState *inspectOwned(Invocation &ctx, const OwnedDescriptor &value) noexcept {
  const auto &s = value.storage;
  if (!s.allocated || s.aligned != s.allocated || s.offset || s.strides[0] != 1 ||
      s.sizes[0] < std::int64_t(sizeof(OwnedState))) {
    ctx.fault(3);
    return nullptr;
  }
  auto *p = static_cast<OwnedState *>(
      ctx.readResource(s.allocated, OwnedState::cleanup, std::uint64_t(s.sizes[0])));
  if (p && std::uint64_t(s.sizes[0]) != p->bytes) {
    ctx.fault(3);
    return nullptr;
  }
  return p;
}
} // namespace kalandra::runtime
using namespace kalandra::runtime;
namespace {
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
} // namespace
extern "C" void *kalandra_file_scope_enter_v1(Invocation *ctx, const OwnedDescriptor *const *owned,
                                              std::uint64_t count) noexcept {
  if (!healthy(*ctx))
    return nullptr;
  auto *parent = currentScope(*ctx);
  auto *memory = ctx->allocateControl(sizeof(ResourceControl), ResourceControl::cleanup);
  if (!memory)
    return nullptr;
  auto *control = new (memory) ResourceControl(*ctx);
  if (!control->storage.valid())
    return nullptr;
  {
    Invocation::OwnershipSelection selected(control->storage);
    auto *data = ctx->allocateResource(sizeof(ResourceScope), ResourceScope::cleanup);
    if (!data)
      return nullptr;
    control->scope = new (data) ResourceScope;
    control->scope->context = ctx;
    control->scope->control = control;
    if (parent) {
      control->scope->parent = parent->scope;
      auto lease = ctx->lease(parent->scope);
      if (!lease) {
        ctx->fault(3);
        return nullptr;
      }
      control->scope->parentOwner = std::move(*lease);
    }
    for (std::uint64_t n = 0; n < count; ++n) {
      if (!owned || !owned[n]) {
        ctx->fault(3);
        return nullptr;
      }
      auto *file = inspectOwned(*ctx, *owned[n]);
      if (!file)
        return nullptr;
      adopt(*ctx, *file, control->scope);
    }
  }
  return healthy(*ctx) ? control : nullptr;
}
extern "C" void kalandra_file_scope_leave_v1(Invocation *ctx, void *scope,
                                             const OwnedDescriptor *escaping) noexcept {
  if (!scope)
    return;
  auto *control = static_cast<ResourceControl *>(scope);
  if (escaping && healthy(*ctx)) {
    if (auto *file = inspectOwned(*ctx, *escaping))
      adopt(*ctx, *file, control->scope->parent);
  }
  ctx->releaseControl(scope);
}
extern "C" bool kalandra_file_receive_v1(Invocation *ctx, const OwnedDescriptor *value) noexcept {
  auto *file = inspectOwned(*ctx, *value);
  if (!file)
    return false;
  bool unowned;
  {
    std::lock_guard lock(resourceMutex());
    unowned = file->scope == nullptr && !file->retired.load(std::memory_order_acquire);
  }
  // A returned worker value has left its producer's logical scope. Adopt it in
  // its receiver; ordinary borrowed task parameters keep their existing owner.
  if (unowned)
    if (auto *scope = currentScope(*ctx))
      adopt(*ctx, *file, scope->scope);
  return healthy(*ctx);
}
