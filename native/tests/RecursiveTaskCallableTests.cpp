#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
using Code = std::int64_t (*)(const void *, Invocation *);
struct Closure {
  Code code;
  const void *environment;
};
struct List {
  std::int64_t tag, head;
  const void *tail;
};
struct Captures {
  StringDescriptor text;
  List list;
};
static_assert(sizeof(Captures) == 48 && sizeof(Closure) == 16);
extern "C" {
const void *make_nested_list(Invocation *);
std::int64_t task_closure_target(const void *, Invocation *);
std::int64_t task_stateless_target(const void *, Invocation *);
bool task_copy_closure(Invocation *, Closure *, const Invocation *, const Closure *);
bool task_copy_stateless(Invocation *, Closure *, const Invocation *, const Closure *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static const void *code(Code function) { return reinterpret_cast<const void *>(function); }
static Closure fixture(Invocation &source, bool bind = true) {
  static const char key = 0;
  Captures captures{};
  kalandra_string_literal_v1(&source, &captures.text, "é", 2);
  auto root = source.lease(make_nested_list(&source));
  require(bool(root), "closure list fixture missing");
  std::memcpy(&captures.list, root->bytes().data(), sizeof(List));
  const void *dependencies[]{captures.text.allocated, captures.list.tail};
  auto *environment = source.makeRecursiveValueFromPointers(std::as_bytes(std::span{&captures, 1}),
                                                            &key, dependencies);
  require(environment &&
              (!bind || source.bindRecursiveEnvironment(environment, code(task_closure_target))),
          "closure environment fixture failed");
  return {task_closure_target, environment};
}
static void verify(Invocation &context, const Closure &closure) {
  const auto *captures = static_cast<const Captures *>(
      context.readRecursiveEnvironment(closure.environment, sizeof(Captures), code(closure.code)));
  require(captures && captures->text.byteLength == 2 && !std::memcmp(captures->text.data, "é", 2),
          "retained closure String buffer was lost");
  require(closure.code == task_closure_target &&
              closure.code(closure.environment, &context) == 44 && !context.status(),
          "retained task closure could not execute its captures");
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "task closure ownership leaked");
}
static bool copy(Invocation &destination, void *output, const Invocation &source,
                 const void *input) noexcept {
  return task_copy_closure(&destination, static_cast<Closure *>(output), &source,
                           static_cast<const Closure *>(input));
}
static void nothing(void *) noexcept {}
static const ValueLayout layout{85001, sizeof(Closure), alignof(Closure), copy, nothing};
static bool construct(Invocation &destination, void *frame, const Packet &packet) {
  return packet.source && copy(destination, frame, *packet.source, packet.data);
}
static void resume(Task &task) {
  verify(task.context(), *static_cast<const Closure *>(task.frame()));
  task.complete(task.frame());
}
static const TaskEntry entry{
    1,         85002,   sizeof(Closure), alignof(Closure), sizeof(Closure), alignof(Closure),
    construct, nothing, resume,          &layout};
int main() {
  try {
    for (unsigned mode = 0; mode < 18; ++mode) {
      Invocation source, destination, foreign;
      auto input = fixture(source, mode != 5 && mode != 17);
      if (mode == 1)
        input.code = task_stateless_target;
      if (mode == 2)
        input.environment = nullptr;
      if (mode == 3)
        input.environment = reinterpret_cast<void *>(0x1230);
      if (mode == 4)
        input.environment = fixture(foreign).environment;
      if (mode == 6)
        input.environment = source.allocateEnvironment(sizeof(Captures), code(input.code));
      if (mode == 7) {
        static const char key = 0;
        std::int64_t small = 42;
        input.environment = source.makeRecursiveValue(std::as_bytes(std::span{&small, 1}), &key);
        require(source.bindRecursiveEnvironment(input.environment, code(input.code)),
                "small environment binding");
      }
      std::atomic<bool> cancelled{mode == 8};
      destination.bindCancellation(&cancelled);
      if (mode == 9)
        destination.fault(5);
      if (mode == 10)
        destination.expectedTaskError(1, 2, 0, 0);
      if (mode == 11)
        destination.expectedApplicationError("Problem", "Invalid");
      if (mode == 12)
        destination.settle();
      if (mode == 13)
        source.fault(5);
      if (mode == 14)
        source.settle();
      if (mode == 15)
        input.code = nullptr;
      if (mode == 16)
        input.code = reinterpret_cast<Code>(0x1230);
      if (mode == 17)
        require(source.bindRecursiveEnvironment(input.environment, code(task_stateless_target)),
                "wrong-code binding fixture");
      const auto beforeInput = input;
      Closure output{task_stateless_target, reinterpret_cast<void *>(0x5a5a)};
      const auto before = output;
      const bool ok = task_copy_closure(&destination, &output, &source, &input);
      require(ok == (mode == 0), "invalid task closure accepted");
      require(input.code == beforeInput.code && input.environment == beforeInput.environment,
              "closure input changed");
      if (ok) {
        require(output.environment == input.environment, "immutable environment was cloned");
        source.settle();
        verify(destination, output);
      } else {
        require(output.code == before.code && output.environment == before.environment &&
                    !destination.live(),
                "failed closure handoff published output or owners");
        if (mode == 9)
          require(destination.status() == 5, "existing fault replaced");
        if (mode == 10 || mode == 11)
          require(destination.hasExpectedError() && !destination.status(),
                  "expected error replaced");
      }
      source.settle();
      destination.settle();
      foreign.settle();
      balanced(source);
      balanced(destination);
      balanced(foreign);
    }
    bool succeeded = false;
    for (unsigned attempt = 1; attempt <= 10 && !succeeded; ++attempt) {
      Invocation source, destination;
      auto input = fixture(source);
      auto observer = source.lease(input.environment);
      destination.failAllocationAt(attempt);
      Closure output{};
      succeeded = task_copy_closure(&destination, &output, &source, &input);
      if (succeeded) {
        source.settle();
        verify(destination, output);
      } else {
        require(destination.status() == 2 && !output.code && !output.environment &&
                    !destination.live(),
                "failed environment graph handoff did not roll back");
        require(observer->referenceCount() == 2, "failed handoff retained environment");
      }
      source.settle();
      destination.settle();
      require(observer->referenceCount() == 1, "environment reference survived cleanup");
      balanced(source);
      balanced(destination);
    }
    require(succeeded, "environment allocation sweep never completed");
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation source, destination;
      Closure input{task_stateless_target, nullptr}, output{};
      if (mode == 1)
        input.environment = reinterpret_cast<void *>(0x1230);
      if (mode == 2)
        input.code = task_closure_target;
      std::atomic<bool> cancelled{mode == 3};
      destination.bindCancellation(&cancelled);
      require(task_copy_stateless(&destination, &output, &source, &input) == (mode == 0),
              "stateless closure validation failed");
      if (!mode)
        require(output.code(output.environment, &destination) == 7 && !destination.live(),
                "stateless closure changed");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    // Runtime bridge also checks compiler metadata; it never dereferences unknown payloads.
    for (unsigned mode = 0; mode < 5; ++mode) {
      Invocation source, destination;
      auto input = fixture(source);
      require(!kalandra_recursive_environment_transfer_v1(
                  &destination, mode == 0 ? nullptr : &source, input.environment,
                  mode == 1 ? sizeof(Captures) - 1 : sizeof(Captures),
                  mode == 2   ? 3
                  : mode == 3 ? 32
                              : alignof(Captures),
                  mode == 4 ? nullptr : code(input.code)) &&
                  destination.status() == 3 && !destination.live(),
              "invalid environment metadata admitted");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "task closure core creation failed");
      Invocation source, observed, survivor;
      auto input = fixture(source);
      auto group = core->openRoot();
      auto task = core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
      source.settle();
      require(task && core->drain() && core->failure(task).kind == TaskFailure::None,
              "closure task execution failed");
      Closure first{}, second{};
      require(core->observe(task, layout, observed, &first), "closure observation failed");
      const auto owners = observed.live();
      require(core->observe(task, layout, observed, &second) && observed.live() == owners &&
                  core->observe(task, layout, survivor, &second),
              "repeated closure observation failed");
      require(core->close(group) && core->drain() && core->release(group),
              "closure group cleanup failed");
      const auto stats = core->stats();
      require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups,
              "closure scheduler leaked");
      core.reset();
      verify(observed, first);
      verify(survivor, second);
      // An onward independent local handoff still uses the same bound environment.
      Invocation onward;
      Closure third{};
      require(task_copy_closure(&onward, &third, &survivor, &second),
              "onward closure handoff failed");
      observed.settle();
      survivor.settle();
      verify(onward, third);
      onward.settle();
      balanced(source);
      balanced(observed);
      balanced(survivor);
      balanced(onward);
    }
    std::cout << "recursive task callables passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
