#include "../runtime/NativeSubscription.h"
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <thread>

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
// The releasing thread and either reader can reach the lifetime lock first.
std::atomic<unsigned> racedDisposals{0};
void disposeAfterReaders(std::uint64_t token) {
  require(token == 42);
  ++racedDisposals;
}
void raceRetirementWithReaders() {
  for (unsigned i = 0; i < 32; ++i) {
    Invocation ctx;
    OwnedDescriptor object{};
    auto *state = kalandra_subscription_prepare_v1(&ctx, disposeAfterReaders);
    require(state && kalandra_subscription_publish_v1(&ctx, state, 42, &object));
    std::uint64_t token;
    auto *first = kalandra_handle_borrow_v1(&ctx, &object, &token, 0);
    auto *second = kalandra_handle_borrow_v1(&ctx, &object, &token, 0);
    require(first && second && token == 42);
    std::barrier gate(3);
    auto finish = [&](void *reader) {
      gate.arrive_and_wait();
      kalandra_handle_end_borrow_v1(reader);
    };
    std::thread a(finish, first), b(finish, second);
    gate.arrive_and_wait();
    require(kalandra_subscription_close_v1(&ctx, &object));
    a.join();
    b.join();
    require(racedDisposals == i + 1);
  }
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
    auto *first = kalandra_handle_borrow_v1(&ctx, &object, &token, 0);
    auto *second = kalandra_handle_borrow_v1(&ctx, &object, &token, 0);
    require(first && second && token == 0);
    require(kalandra_subscription_close_v1(&ctx, &object));
    require(calls == 2);
    require(!kalandra_handle_borrow_v1(&ctx, &object, &token, 0));
    ctx.settle(); // The foreign call can finish after the invocation releases its backing.
    require(calls == 2);
    kalandra_handle_end_borrow_v1(first);
    require(calls == 2);
    kalandra_handle_end_borrow_v1(second);
    require(calls == 3);
  }
  {
    Invocation ctx;
    OwnedDescriptor object{};
    auto *state = kalandra_subscription_prepare_v1(&ctx, dispose);
    require(state && kalandra_subscription_publish_v1(&ctx, state, 0, &object));
    std::uint64_t token;
    auto *writer = kalandra_handle_borrow_v1(&ctx, &object, &token, 1);
    require(writer);
    require(!kalandra_handle_borrow_v1(&ctx, &object, &token, 0));
    kalandra_handle_end_borrow_v1(writer);
  }
  require(calls == 4);
  raceRetirementWithReaders();
}
