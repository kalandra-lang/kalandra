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
thread_local const OwnedReceiveGuard *ownedReceive = nullptr;
} // namespace
std::mutex &resourceMutex() {
  static auto *value = new std::mutex;
  return *value;
}
OwnedReceiveGuard::OwnedReceiveGuard(const Invocation &source, Invocation &destination) noexcept
    : previous_(ownedReceive), source_(&source), destination_(&destination) {
  ownedReceive = this;
}
OwnedReceiveGuard::~OwnedReceiveGuard() { ownedReceive = previous_; }
bool OwnedReceiveGuard::receives(Invocation &destination, const Invocation &source,
                                  const ResourceScope *scope) noexcept {
  return ownedReceive && ownedReceive->source_ == &source &&
         ownedReceive->destination_ == &destination && scope &&
         scope->context == ownedReceive->source_ && scope->context != &destination;
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
ResourceControl *ensureOwnedEscapeScope(Invocation &ctx) noexcept {
  if (ctx.poll() || !ctx.usable())
    return nullptr;
  auto *oldest = currentScope(ctx);
  if (oldest) {
    while (oldest->scope->parent)
      oldest = oldest->scope->parent->control;
    if (!oldest->scope->active || oldest->scope->retiring || oldest->scope->context != &ctx) {
      ctx.fault(3);
      return nullptr;
    }
    if (oldest->scope->escapeRoot)
      return oldest;
  }
  auto *memory = ctx.allocateControl(sizeof(ResourceControl), ResourceControl::cleanup);
  if (!memory)
    return nullptr;
  auto *root = new (memory) ResourceControl(ctx);
  if (!root->storage.valid()) {
    ctx.releaseControl(root);
    return nullptr;
  }
  {
    Invocation::OwnershipSelection selected(root->storage);
    auto *data = ctx.allocateResource(sizeof(ResourceScope), ResourceScope::cleanup);
    if (data) {
      root->scope = new (data) ResourceScope;
      root->scope->context = &ctx;
      root->scope->control = root;
      root->scope->escapeRoot = true;
    }
  }
  if (!root->scope || ctx.poll() || !ctx.usable()) {
    // Still the newest control: rollback cannot retire existing user scopes.
    ctx.releaseControl(root);
    return nullptr;
  }
  auto parent = ctx.lease(root->scope);
  if (!parent || !ctx.moveNewestControlToRoot(root)) {
    ctx.fault(3);
    ctx.releaseControl(root);
    return nullptr;
  }
  if (oldest) {
    oldest->scope->parent = root->scope;
    oldest->scope->parentOwner = std::move(*parent);
  }
  return root;
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
bool receiveOwned(Invocation &ctx, OwnedState &file, const Invocation *source) noexcept {
  bool unowned, transferring;
  {
    std::lock_guard lock(resourceMutex());
    const bool retired = file.retired.load(std::memory_order_acquire);
    if (file.deferredOwner && (retired || (file.scope && file.scope->retiring))) {
      ctx.fault(3);
      return false;
    }
    unowned = file.scope == nullptr && !retired;
    transferring = source && file.deferredOwner && !retired &&
                   OwnedReceiveGuard::receives(ctx, *source, file.scope);
  }
  if (file.deferredOwner && (unowned || transferring)) {
    if (!ensureOwnedEscapeScope(ctx))
      return false;
    adopt(ctx, file, currentScope(ctx)->scope);
  } else if (unowned)
    if (auto *scope = currentScope(ctx))
      adopt(ctx, file, scope->scope);
  return healthy(ctx);
}
} // namespace
extern "C" bool kalandra_owned_copy_v1(Invocation *ctx, const Invocation *source,
                                         const OwnedDescriptor *value) noexcept {
  if (!ctx)
    return false;
  if (!healthy(*ctx))
    return false;
  if (!source || !value) {
    ctx->fault(3);
    return false;
  }
  const auto &s = value->storage;
  if (!s.allocated || s.aligned != s.allocated || s.offset || s.strides[0] != 1 ||
      s.sizes[0] < std::int64_t(sizeof(OwnedState))) {
    ctx->fault(3);
    return false;
  }
  auto *state = static_cast<OwnedState *>(
      ctx->readResourceFrom(*source, s.allocated, OwnedState::cleanup, std::uint64_t(s.sizes[0])));
  if (!state)
    return false;
  if (state->bytes != std::uint64_t(s.sizes[0])) {
    ctx->fault(3);
    return false;
  }
  if (state->deferredOwner) {
    if (state->retired.load(std::memory_order_acquire)) {
      ctx->fault(3);
      return false;
    }
    if (!ensureOwnedEscapeScope(*ctx))
      return false;
    auto *scope = currentScope(*ctx);
    // Keep one dynamic accounting path per logical store rather than adding a
    // temporary value-store observer at every nested owned-result boundary.
    Invocation::OwnershipSelection selected(scope->storage);
    return ctx->retainFrom(*source, s.allocated) && receiveOwned(*ctx, *state, source);
  }
  return ctx->retainFrom(*source, s.allocated) && receiveOwned(*ctx, *state, source);
}
extern "C" void *kalandra_file_scope_enter_v1(Invocation *ctx, const OwnedDescriptor *const *owned,
                                              std::uint64_t count) noexcept {
  if (!healthy(*ctx))
    return nullptr;
  auto *parent = currentScope(*ctx);
  auto *memory = ctx->allocateControl(sizeof(ResourceControl), ResourceControl::cleanup);
  if (!memory)
    return nullptr;
  auto *control = new (memory) ResourceControl(*ctx);
  if (!control->storage.valid()) {
    ctx->releaseControl(control);
    return nullptr;
  }
  {
    Invocation::OwnershipSelection selected(control->storage);
    if (auto *data = ctx->allocateResource(sizeof(ResourceScope), ResourceScope::cleanup)) {
      control->scope = new (data) ResourceScope;
      control->scope->context = ctx;
      control->scope->control = control;
      if (parent) {
        control->scope->parent = parent->scope;
        if (auto lease = ctx->lease(parent->scope))
          control->scope->parentOwner = std::move(*lease);
        else
          ctx->fault(3);
      }
    }
  }
  if (!control->scope || !healthy(*ctx)) {
    // No capture has moved yet. A half-constructed scope must not hide older
    // asynchronous owners from failure maintenance's lexical-scope traversal.
    ctx->releaseControl(control);
    return nullptr;
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
    if (!healthy(*ctx))
      return nullptr; // keep valid scope and any already-moved captures for cleanup
  }
  return control;
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
  // A returned worker value has left its producer's logical scope. Adopt it in
  // its receiver. This legacy callback has no source identity, so it cannot
  // consume an existing owner through an enclosing result-copy permission.
  return receiveOwned(*ctx, *file, nullptr);
}

extern "C" bool kalandra_owned_adopt_v1(Invocation *ctx, const OwnedDescriptor *value) noexcept {
  if (!ctx || !value || !healthy(*ctx))
    return false;
  auto *state = inspectOwned(*ctx, *value);
  auto *control = currentScope(*ctx);
  bool foreignOwner = false, alreadyLocal = false;
  if (state) {
    std::lock_guard lock(resourceMutex());
    foreignOwner = state->scope && state->scope->context != ctx;
    alreadyLocal = control && state->scope == control->scope && state->scope &&
                   state->scope->active && !state->scope->retiring &&
                   !state->retired.load(std::memory_order_acquire);
  }
  if (!state || !control || !control->scope || foreignOwner) {
    ctx->fault(3);
    return false;
  }
  if (!alreadyLocal)
    adopt(*ctx, *state, control->scope);
  return healthy(*ctx);
}
