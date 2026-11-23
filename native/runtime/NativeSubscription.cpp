#include "NativeSubscription.h"

namespace kalandra::runtime {
namespace {
using Release = void (*)(std::uint64_t);
struct Subscription final : OwnedState {
  Release release;
  std::uint64_t token = 0;
  // This lock protects the native token. The OwnedState lock independently
  // protects scope retirement, which can be reentrant from a native callback.
  std::mutex lifetime;
  std::size_t borrowers = 0;
  bool armed = false;
  explicit Subscription(Release fn) noexcept
      : OwnedState(OwnedKind::NativeSubscription, sizeof(Subscription), destroySubscription, close),
        release(fn) {}
  static void close(void *p) noexcept {
    auto &s = *static_cast<Subscription *>(p);
    {
      std::lock_guard lock(s.lifetime);
      if (!s.armed)
        return;
      s.armed = false;
      if (s.borrowers)
        return;
    }
    s.release(s.token);
  }
  static void destroySubscription(void *p) noexcept {
    close(p);
    static_cast<Subscription *>(p)->~Subscription();
  }
};
} // namespace
// Admit the owner before the adapter can register anything. The generated code
// calls publish in the same scheduler step, even if cancellation arrives meanwhile.
extern "C" void *kalandra_subscription_prepare_v1(Invocation *ctx, Release release) noexcept {
  if (ctx->poll() || !ctx->usable())
    return nullptr;
  auto *memory = ctx->allocateResource(sizeof(Subscription), OwnedState::cleanup, true);
  if (!memory)
    return nullptr;
  auto *state = new (memory) Subscription(release);
  return ctx->trackResource(state) ? state : nullptr;
}
extern "C" bool kalandra_subscription_publish_v1(Invocation *ctx, void *memory, std::uint64_t token,
                                                 OwnedDescriptor *out) noexcept {
  auto *s = static_cast<Subscription *>(memory);
  s->token = token;
  s->armed = true;
  if (auto *scope = currentScope(*ctx))
    adopt(*ctx, *s, scope->scope);
  if (ctx->poll() || !ctx->usable()) {
    Subscription::close(s);
    return false;
  }
  *out = ownedDescriptor(s);
  return true;
}
// The instruction owns its input descriptor through end_borrow. Closing the
// logical owner during a call prevents new borrows and defers disposal.
extern "C" void *kalandra_handle_borrow_v1(Invocation *ctx, const OwnedDescriptor *value,
                                            std::uint64_t *token) noexcept {
  if (ctx->poll() || !ctx->usable())
    return nullptr;
  auto *owned = inspectOwned(*ctx, *value);
  if (owned && owned->kind == OwnedKind::NativeSubscription) {
    auto *s = static_cast<Subscription *>(owned);
    std::lock_guard lock(s->lifetime);
    if (s->armed && !s->retired.load(std::memory_order_acquire)) {
      ++s->borrowers;
      *token = s->token;
      return s;
    }
  }
  ctx->fault(3);
  return nullptr;
}
extern "C" void kalandra_handle_end_borrow_v1(void *state) noexcept {
  auto *s = static_cast<Subscription *>(state);
  {
    std::lock_guard lock(s->lifetime);
    if (--s->borrowers || s->armed)
      return;
  }
  s->release(s->token);
}
extern "C" bool kalandra_subscription_close_v1(Invocation *ctx,
                                               const OwnedDescriptor *value) noexcept {
  auto *s = inspectOwned(*ctx, *value);
  if (!s || s->kind != OwnedKind::NativeSubscription) {
    ctx->fault(3);
    return false;
  }
  Subscription::close(s);
  return true;
}
} // namespace kalandra::runtime
