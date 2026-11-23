#include "NativeSubscription.h"

namespace kalandra::runtime {
namespace {
using Release = void (*)(std::uint64_t);
struct Subscription final : OwnedState {
  Release release;
  std::uint64_t token = 0;
  std::atomic<bool> armed{false};
  explicit Subscription(Release fn) noexcept
      : OwnedState(OwnedKind::NativeSubscription, sizeof(Subscription), destroySubscription, close),
        release(fn) {}
  static void close(void *p) noexcept {
    auto &s = *static_cast<Subscription *>(p);
    if (s.armed.exchange(false, std::memory_order_acq_rel))
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
  s->armed.store(true, std::memory_order_release);
  if (auto *scope = currentScope(*ctx))
    adopt(*ctx, *s, scope->scope);
  if (ctx->poll() || !ctx->usable()) {
    Subscription::close(s);
    return false;
  }
  *out = ownedDescriptor(s);
  return true;
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
