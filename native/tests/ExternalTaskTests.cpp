#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "External task assertion %d\n", __LINE__);                              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
template <class P> void until(P predicate) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<int *>(out) = *static_cast<const int *>(in);
  return true;
}
void destroyValue(void *) noexcept {}
const ValueLayout layout{19001, sizeof(int), alignof(int), copy, destroyValue};
struct Shared {
  std::mutex mutex;
  ExternalCompletion token;
  std::atomic<unsigned> suspended{0}, destroyed{0}, plain{0};
  std::atomic<bool> hold{false}, release{false};
  bool immediate = false, fault = false, before = false;
  std::atomic<bool> atReservation{false};
  unsigned rounds = 1;
};
struct Input {
  Shared *shared;
  bool plain = false;
};
struct Frame {
  Input input;
  unsigned round = 0;
};
bool construct(Invocation &, void *out, const Packet &packet) {
  new (out) Frame{*static_cast<const Input *>(packet.data)};
  return true;
}
void destroy(void *in) noexcept {
  auto *f = static_cast<Frame *>(in);
  ++f->input.shared->destroyed;
  f->~Frame();
}
void resume(Task &task) {
  auto &frame = *static_cast<Frame *>(task.frame());
  auto &s = *frame.input.shared;
  if (frame.input.plain) {
    ++s.plain;
    int result = 9;
    task.complete(&result);
    return;
  }
  if (frame.round++ < s.rounds) {
    if (s.before) {
      s.atReservation = true;
      until([&] { return s.release.load(); });
    }
    auto token = task.suspendExternal();
    if (s.before) {
      CHECK(!token);
      return;
    }
    CHECK(token);
    {
      std::lock_guard lock(s.mutex);
      s.token = token;
    }
    ++s.suspended;
    if (s.immediate)
      CHECK(token.signal());
    if (s.fault)
      task.fault(71);
    if (s.hold)
      until([&] { return s.release.load(); });
    return;
  }
  int result = 42;
  task.complete(&result);
}
const TaskEntry entry{
    1,         19002,   sizeof(Input), alignof(Input), sizeof(Frame), alignof(Frame),
    construct, destroy, resume,        &layout};
TaskHandle submit(TaskCore &core, GroupHandle group, Shared &s, bool plain = false) {
  Input input{&s, plain};
  return core.submit(group, entry, {19002, &input, sizeof(input)});
}
ExternalCompletion token(Shared &s) {
  std::lock_guard lock(s.mutex);
  return s.token;
}
void clean(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.settled(group));
  CHECK(core.release(group));
  auto stats = core.stats();
  CHECK(!stats.records && !stats.groups && !stats.retainedBytes && !stats.unfinished &&
        !stats.externalPending);
}
int main() {
  CHECK(ExternalCompletion{}.cancelled());
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.ready = 1;
    // A suspended external operation does not occupy the sole worker or ready slot.
    {
      auto core = TaskCore::create(limits);
      CHECK(core);
      Shared s;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      CHECK(core->step());
      CHECK(core->stats().externalPending == 1);
      auto plain = submit(*core, group, s, true);
      CHECK(plain);
      CHECK(core->step());
      CHECK(s.plain == 1);
      CHECK(!core->settled(task));
      Invocation context;
      int value = 0;
      CHECK(core->observe(plain, layout, context, &value) && value == 9);
      auto completion = token(s);
      CHECK(!completion.cancelled());
      CHECK(completion.signal());
      CHECK(completion.cancelled());
      CHECK(!completion.signal());
      CHECK(core->drain());
      CHECK(core->observe(task, layout, context, &value) && value == 42);
      clean(*core, group);
      CHECK(!completion.signal());
    }
    // Cancellation/deadlines cannot free a frame until the external user acknowledges completion.
    for (bool deadline : {false, true}) {
      auto core = TaskCore::create(limits);
      Shared s;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      CHECK(core->step());
      CHECK(!token(s).cancelled());
      CHECK(core->cancel(group, deadline));
      CHECK(token(s).cancelled());
      CHECK(!core->drain(0));
      CHECK(!core->settled(task) && s.destroyed == 0);
      auto completion = token(s);
      CHECK(completion.signal());
      CHECK(core->drain());
      CHECK(core->failure(group).kind ==
            (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      clean(*core, group);
      CHECK(s.destroyed == 1);
    }
    // Callback arrival cannot re-enter a frame while its original execution lease is still live.
    {
      auto core = TaskCore::create(limits);
      Shared s;
      s.hold = true;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      std::jthread driver([&] { CHECK(core->drain()); });
      until([&] { return s.suspended.load() == 1; });
      auto completion = token(s);
      CHECK(completion.signal());
      CHECK(!core->step());
      CHECK(s.destroyed == 0);
      s.release = true;
      driver.join();
      Invocation context;
      int value = 0;
      CHECK(core->observe(task, layout, context, &value) && value == 42);
      clean(*core, group);
    }
    // A latched fault still pins externally used storage and keeps the original failure.
    {
      auto core = TaskCore::create(limits);
      Shared s;
      s.fault = true;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      CHECK(core->step());
      CHECK(!core->settled(task) && s.destroyed == 0);
      CHECK(token(s).signal());
      CHECK(core->drain());
      CHECK(core->failure(task).code == 71);
      clean(*core, group);
    }
    // Successive suspensions reject the previous epoch, and callbacks may finish before resume
    // returns.
    for (bool immediate : {false, true}) {
      auto core = TaskCore::create(limits);
      Shared s;
      s.rounds = 2;
      s.immediate = immediate;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      ExternalCompletion first;
      if (immediate)
        CHECK(core->drain());
      else {
        CHECK(core->step());
        first = token(s);
        CHECK(first.signal());
        CHECK(core->step());
        CHECK(!first.signal());
        CHECK(first.cancelled());
        CHECK(!token(s).cancelled());
        CHECK(token(s).signal());
        CHECK(core->drain());
      }
      Invocation context;
      int value = 0;
      CHECK(core->observe(task, layout, context, &value));
      clean(*core, group);
    }
    // A cancellation already recorded at reservation never creates an external lease.
    {
      auto core = TaskCore::create(limits);
      Shared s;
      s.before = true;
      auto group = core->openRoot();
      auto task = submit(*core, group, s);
      std::jthread driver([&] { CHECK(core->drain()); });
      until([&] { return s.atReservation.load(); });
      CHECK(core->cancel(group));
      s.release = true;
      driver.join();
      CHECK(core->stats().externalPending == 0 &&
            core->failure(task).kind == TaskFailure::Cancelled);
      clean(*core, group);
    }
    // Core destruction waits for acknowledgement; late copied tokens are harmless afterward.
    {
      auto core = TaskCore::create(limits);
      Shared s;
      auto group = core->openRoot();
      submit(*core, group, s);
      CHECK(core->step());
      auto completion = token(s);
      std::atomic<bool> dropping = false, done = false;
      std::jthread drop([&] {
        dropping = true;
        core.reset();
        done = true;
      });
      until([&] { return dropping.load(); });
      CHECK(s.destroyed == 0);
      CHECK(completion.signal());
      drop.join();
      CHECK(done && s.destroyed == 1);
      CHECK(!completion.signal());
    }
  }
  std::puts("External completion leases, cancellation, early/stale callbacks, and shutdown passed");
}
