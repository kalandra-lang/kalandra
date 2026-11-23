#include "../runtime/NativeSubscription.h"
#include <atomic>
#include <cstdlib>

using namespace kalandra::runtime;
namespace {
unsigned calls;
thread_local Invocation *reenter;
thread_local const OwnedDescriptor *active;
void require(bool value) {
  if (!value)
    std::abort();
}
void dispose(std::uint64_t token) {
  require(token == 0); // Zero can be a valid native registration identifier.
  ++calls;
  if (reenter)
    require(kalandra_subscription_close_v1(reenter, active));
}
} // namespace
int main() {
  {
    Invocation ctx;
    OwnedDescriptor subscription{};
    auto *storage = kalandra_subscription_prepare_v1(&ctx, dispose);
    require(storage && kalandra_subscription_publish_v1(&ctx, storage, 0, &subscription));
    reenter = &ctx;
    active = &subscription;
    require(kalandra_subscription_close_v1(&ctx, &subscription));
    reenter = nullptr;
    active = nullptr;
    require(calls == 1);
  }
  require(calls == 1);
  {
    Invocation ctx;
    std::atomic<bool> cancelled{false};
    ctx.bindCancellation(&cancelled);
    OwnedDescriptor unpublished{};
    auto *storage = kalandra_subscription_prepare_v1(&ctx, dispose);
    require(storage);
    cancelled.store(true);
    require(!kalandra_subscription_publish_v1(&ctx, storage, 0, &unpublished));
    require(!unpublished.storage.allocated && calls == 2);
    require(!kalandra_subscription_prepare_v1(&ctx, dispose));
  }
  require(calls == 2);
  {
    Invocation ctx;
    OwnedDescriptor object{};
    auto *state = kalandra_subscription_prepare_v1(&ctx, dispose);
    require(state && kalandra_subscription_publish_v1(&ctx, state, 0, &object));
    std::uint64_t token = 123;
    auto *first = kalandra_handle_borrow_v1(&ctx, &object, &token);
    auto *second = kalandra_handle_borrow_v1(&ctx, &object, &token);
    require(first && second && token == 0);
    require(kalandra_subscription_close_v1(&ctx, &object));
    require(calls == 2);
    require(!kalandra_handle_borrow_v1(&ctx, &object, &token));
    kalandra_handle_end_borrow_v1(first);
    require(calls == 2);
    kalandra_handle_end_borrow_v1(second);
    require(calls == 3);
  }
  require(calls == 3);
}
