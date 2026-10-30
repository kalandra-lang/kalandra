#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
using Code = std::int64_t (*)(const void *, std::int64_t, Invocation *);
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
struct Tree {
  StringDescriptor name;
  StringArrayDescriptor children;
};
struct CallableTree {
  std::int64_t tag;
  Closure apply;
  StringArrayDescriptor items, children;
  List list;
};
template <class T> struct Result {
  Failure failure;
  T value;
};
struct Buffer {
  alignas(std::max_align_t) std::array<std::byte, 512> bytes{};
  template <class T> T &get() { return *reinterpret_cast<T *>(bytes.data()); }
  void *data() { return bytes.data(); }
};
static_assert(sizeof(Result<List>) == 56 && sizeof(Result<Tree>) == 96 &&
              sizeof(Result<CallableTree>) == 160);
extern "C" {
const void *make_nested_list(Invocation *);
const void *callable_box(const void *, const void *, std::int64_t, Invocation *);
bool task_project_tail(Invocation *, const List *, List *);
std::int64_t kalandra_callable_0(const void *, std::int64_t, Invocation *);
std::int64_t kalandra_callable_1(const void *, std::int64_t, Invocation *);
extern const TaskEntry *recursive_generated_entry_0;
extern const TaskEntry *recursive_generated_entry_1;
extern const TaskEntry *recursive_generated_entry_2;
extern const TaskEntry *recursive_generated_entry_3;
extern const TaskEntry *recursive_generated_entry_4;
extern const TaskEntry *recursive_generated_entry_5;
extern const TaskEntry *recursive_generated_entry_6;
extern const TaskEntry *recursive_generated_entry_7;
extern const TaskEntry *recursive_generated_entry_9;
extern const TaskEntry *recursive_generated_entry_10;
const void *task_box_created_environment(const CallableTree *, Invocation *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "generated entry invocation leaked");
}
static void clean(const TaskCore &core) {
  const auto stats = core.stats();
  require(!stats.retainedOwners && !stats.retainedBytes && !stats.environmentBytes &&
              !stats.recursiveBytes && !stats.records && !stats.groups && !stats.unfinished &&
              !stats.pending && !stats.externalPending,
          "generated entry scheduler leaked");
}
static List list(Invocation &context) {
  auto root = context.lease(make_nested_list(&context));
  require(bool(root), "generated entry List fixture missing");
  List value;
  std::memcpy(&value, root->bytes().data(), sizeof(value));
  return value;
}
static Closure closure(Invocation &context) {
  static const char key = 0;
  Captures captures{};
  kalandra_string_literal_v1(&context, &captures.text, "é", 2);
  captures.list = list(context);
  const void *dependencies[]{captures.text.allocated, captures.list.tail};
  auto *environment = context.makeRecursiveValueFromPointers(std::as_bytes(std::span{&captures, 1}),
                                                             &key, dependencies);
  require(environment && context.bindRecursiveEnvironment(
                             environment, reinterpret_cast<const void *>(kalandra_callable_0)),
          "generated entry closure fixture failed");
  return {kalandra_callable_0, environment};
}
static void fixture(unsigned id, Invocation &source, Buffer &packet) {
  if (id == 3) {
    StringDescriptor text;
    kalandra_string_literal_v1(&source, &text, "é", 2);
    auto *child = static_cast<Tree *>(source.allocateRecord(sizeof(Tree)));
    require(child, "generated tree fixture allocation");
    *child = {text, {}};
    packet.get<Result<Tree>>() = {{}, {text, {child, child, 0, 1, 1}}};
  } else if (id == 4) {
    const auto captured = closure(source);
    auto *functions = static_cast<Closure *>(source.allocateRecord(3 * sizeof(Closure)));
    require(functions, "generated callable array allocation");
    functions[0] = functions[1] = captured;
    functions[2] = {kalandra_callable_1, nullptr};
    auto *child = static_cast<CallableTree *>(source.allocateRecord(sizeof(CallableTree)));
    require(child, "generated callable child allocation");
    *child = {2, captured, {functions, functions, 0, 3, 1}, {}, list(source)};
    packet.get<Result<CallableTree>>() = {
        {}, {2, captured, {functions, functions, 0, 3, 1}, {child, child, 0, 1, 1}, list(source)}};
  } else
    packet.get<Result<List>>() = {{}, list(source)};
}
static void verifyList(Invocation &context, const List &value, std::int64_t head) {
  require(value.tag == 2 && value.head == head, "generated List value changed");
  List child{};
  require(task_project_tail(&context, &value, &child) && child.tag == (head == 42 ? 1 : 2),
          "generated List child lost");
}
static void verify(unsigned id, Invocation &context, Buffer &output) {
  require(output.get<Failure>().kind == TaskFailure::None,
          "generated entry returned an unexpected wrapper failure");
  if (id == 2)
    require(output.get<Result<std::int64_t>>().value == 10,
            "generated inline field projection changed");
  else if (id == 3) {
    const auto &tree = output.get<Result<Tree>>().value;
    require(tree.name.byteLength == 2 && !std::memcmp(tree.name.data, "é", 2) &&
                tree.children.size == 1,
            "generated tree backing lost");
    const auto &child = *static_cast<const Tree *>(tree.children.data);
    require(child.name.byteLength == 2 && !std::memcmp(child.name.data, "é", 2) &&
                !child.children.size,
            "generated child tree backing lost");
  } else if (id == 4) {
    const auto &tree = output.get<Result<CallableTree>>().value;
    require(tree.tag == 2 && tree.apply.code(tree.apply.environment, 10, &context) == 54 &&
                tree.items.size == 3,
            "generated callable tree result changed");
    const auto *functions = static_cast<const Closure *>(tree.items.data);
    require(functions[0].environment == functions[1].environment,
            "generated entry cloned immutable environment");
    for (unsigned i = 0; i < 3; ++i)
      require(functions[i].code(functions[i].environment, 10, &context) == (i == 2 ? 20 : 54),
              "generated function array captures lost");
    verifyList(context, tree.list, 10);
    const auto *captures = static_cast<const Captures *>(context.readRecursiveEnvironment(
        tree.apply.environment, sizeof(Captures), reinterpret_cast<const void *>(tree.apply.code)));
    require(captures && !std::memcmp(captures->text.data, "é", 2),
            "generated callable character buffer lost");
  } else
    verifyList(context, output.get<Result<List>>().value, id == 1 ? 42 : 10);
  require(!context.status(), "generated result inspection faulted");
}
static void closureConstructionBudgets() {
  for (unsigned workers : {1u, 2u, 4u}) {
    std::size_t admittedBytes = 0, peakBytes = 0;
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
      TaskLimits limits;
      limits.workers = workers;
      if (boundary)
        limits.bytes = boundary == 1 ? admittedBytes : peakBytes - (boundary == 2 ? 1 : 0);
      auto core = TaskCore::create(limits);
      require(bool(core), "closure budget core missing");
      Invocation source, observer;
      Buffer packet, output;
      fixture(4, source, packet);
      const auto &entry = *recursive_generated_entry_7;
      const auto group = core->openRoot();
      const auto task = core->submit(
          group, entry, {entry.packetLayout, packet.data(), entry.packetBytes, &source});
      if (!boundary)
        admittedBytes = core->stats().retainedBytes;
      else
        require(core->stats().retainedBytes == admittedBytes,
                "closure budget failed before task-created allocation");
      source.settle();
      require(task && core->drain(), "closure budget drain failed");
      if (!boundary)
        peakBytes = core->stats().peakBytes;
      require(peakBytes > admittedBytes && core->stats().peakBytes <= limits.bytes,
              "task-created environment escaped scheduler accounting");
      if (boundary == 1 || boundary == 2) {
        const auto failure = core->failure(task);
        output.get<Result<Closure>>().value.environment = reinterpret_cast<void *>(0x1230);
        require(failure.kind == TaskFailure::Fault && failure.code == 2 &&
                    !core->observe(task, *entry.result, observer, output.data()) &&
                    output.get<Result<Closure>>().value.environment ==
                        reinterpret_cast<void *>(0x1230),
                "over-budget task-created environment published a closure");
      } else {
        require(core->failure(task).kind == TaskFailure::None &&
                    core->observe(task, *entry.result, observer, output.data()),
                "exact closure construction/result budget rejected");
      }
      require(core->close(group) && core->drain() && core->release(group),
              "closure budget cleanup failed");
      clean(*core);
      core.reset();
      if (!boundary || boundary == 3) {
        const auto value = output.get<Result<Closure>>().value;
        require(value.code && value.code(value.environment, 23, &observer) == 10 &&
                    !observer.status(),
                "budgeted closure lost captures after scheduler destruction");
      }
      observer.settle();
      balanced(source);
      balanced(observer);
    }
  }
}
static void createdClosures() {
  for (unsigned workers : {1u, 2u, 4u})
    for (bool stateless : {false, true}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "closure factory core missing");
      Invocation source, first, survivor;
      Buffer packet, output, repeated;
      if (!stateless)
        fixture(4, source, packet);
      const auto &entry = *(stateless ? recursive_generated_entry_10 : recursive_generated_entry_7);
      const auto group = core->openRoot();
      const auto task = core->submit(
          group, entry, {entry.packetLayout, packet.data(), entry.packetBytes, &source});
      source.settle();
      require(task && core->drain() && core->failure(task).kind == TaskFailure::None &&
                  core->observe(task, *entry.result, first, output.data()) &&
                  core->observe(task, *entry.result, first, repeated.data()) &&
                  core->observe(task, *entry.result, survivor, repeated.data()),
              "generated closure construction or observation failed");
      require(core->close(group) && core->drain() && core->release(group),
              "closure factory cleanup failed");
      clean(*core);
      core.reset();
      const auto inspect = [&](Invocation &context, Buffer &buffer) {
        const auto closure = buffer.get<Result<Closure>>().value;
        require(closure.code &&
                    closure.code(closure.environment, 23, &context) == (stateless ? 23 : 10) &&
                    !context.status(),
                "task-created closure lost its captures");
        if (!stateless) {
          const auto *captures = static_cast<const CallableTree *>(
              context.readRecursiveEnvironment(closure.environment, sizeof(CallableTree),
                                               reinterpret_cast<const void *>(closure.code)));
          require(captures && captures->children.size == 1,
                  "task-created environment graph missing");
          Buffer view;
          view.get<Result<CallableTree>>() = {{}, *captures};
          verify(4, context, view);
          view.get<Result<CallableTree>>().value =
              *static_cast<const CallableTree *>(captures->children.data);
          verify(4, context, view);
        } else
          require(!closure.environment, "capture-free task closure allocated an environment");
      };
      inspect(first, output);
      inspect(survivor, repeated);
      first.settle();
      // Transfer the newly created closure through another generated task.
      auto onward = TaskCore::create(limits);
      require(bool(onward), "closure onward core missing");
      const auto &echo = *recursive_generated_entry_9;
      const auto nextGroup = onward->openRoot();
      const auto nextTask = onward->submit(
          nextGroup, echo, {echo.packetLayout, repeated.data(), echo.packetBytes, &survivor});
      survivor.settle();
      Invocation final;
      Buffer result;
      require(nextTask && onward->drain() && onward->failure(nextTask).kind == TaskFailure::None &&
                  onward->observe(nextTask, *echo.result, final, result.data()),
              "task-created closure onward transfer failed");
      require(onward->close(nextGroup) && onward->drain() && onward->release(nextGroup),
              "closure onward cleanup failed");
      clean(*onward);
      onward.reset();
      inspect(final, result);
      if (!stateless) {
        const auto closure = result.get<Result<Closure>>().value;
        for (unsigned mode = 0; mode < 5; ++mode) {
          Invocation invalid;
          const void *environment = mode == 0 ? nullptr : closure.environment;
          if (mode == 2)
            invalid.expectedTaskError(1, 77, 2, 3);
          if (mode == 3)
            invalid.fault(91);
          if (mode == 4)
            invalid.expectedApplicationError("CaptureError", "Stopped");
          require(closure.code(environment, 1, &invalid) == 0,
                  "invalid capture environment executed");
          require(mode == 2   ? invalid.hasTaskError() && !invalid.status()
                  : mode == 4 ? invalid.hasApplicationError() && !invalid.status()
                              : invalid.status() == (mode == 3 ? 91 : 3),
                  "capture read replaced an existing error");
          invalid.settle();
          balanced(invalid);
        }
      }
      if (stateless) {
        Invocation invalid;
        const auto closure = result.get<Result<Closure>>().value;
        require(closure.code(reinterpret_cast<const void *>(0x1234), 23, &invalid) == 0 &&
                    invalid.status() == 3,
                "capture-free generated entry accepted an environment");
        invalid.settle();
        balanced(invalid);
      }
      final.settle();
      balanced(source);
      balanced(first);
      balanced(survivor);
      balanced(final);
    }
  bool succeeded = false;
  unsigned failures = 0;
  for (unsigned attempt = 1; attempt < 64 && !succeeded; ++attempt) {
    Invocation context;
    Buffer input;
    fixture(4, context, input);
    context.failAllocationAt(context.allocations() + attempt);
    const auto *environment =
        task_box_created_environment(&input.get<Result<CallableTree>>().value, &context);
    if (environment)
      succeeded = true;
    else {
      ++failures;
      require(context.status() == 2, "environment publication lost allocation failure");
    }
    context.settle();
    balanced(context);
  }
  require(succeeded && failures >= 2, "environment allocation sweep missed publication stages");
  for (unsigned mode = 0; mode < 4; ++mode) {
    Invocation context;
    Buffer input;
    fixture(4, context, input);
    auto captures = input.get<Result<CallableTree>>().value;
    if (mode == 0)
      captures.tag = (std::int64_t{1} << 32) | 2;
    else if (mode == 1)
      captures.apply.environment = reinterpret_cast<const void *>(0x1234);
    else if (mode == 2)
      captures.list.tail = reinterpret_cast<const void *>(0x1234);
    else
      captures.children.data = reinterpret_cast<void *>(0x1234);
    require(!task_box_created_environment(&captures, &context) && context.status() == 3,
            "invalid capture graph was published");
    context.settle();
    balanced(context);
  }
  {
    Invocation context;
    CallableTree inactive{};
    inactive.tag = 1;
    inactive.apply.environment = reinterpret_cast<const void *>(0x1234);
    inactive.children.data = reinterpret_cast<void *>(0x1234);
    inactive.list.tail = reinterpret_cast<const void *>(0x1234);
    require(task_box_created_environment(&inactive, &context) && !context.status(),
            "inactive capture payload became a dependency");
    context.settle();
    balanced(context);
  }
}
int main() {
  try {
    closureConstructionBudgets();
    createdClosures();
    const std::array entries{recursive_generated_entry_0, recursive_generated_entry_1,
                             recursive_generated_entry_2, recursive_generated_entry_3,
                             recursive_generated_entry_4, recursive_generated_entry_5,
                             recursive_generated_entry_6};
    require(entries[0]->frameBytes == 64 && entries[1]->frameBytes == 120 &&
                entries[2]->frameBytes == 104 && entries[3]->frameBytes == 104 &&
                entries[4]->frameBytes == 168,
            "generated physical frame ABI changed");
    for (unsigned workers : {1u, 2u, 4u})
      for (unsigned id = 0; id < entries.size(); ++id) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "generated entry core creation failed");
        Invocation source, observed, survivor;
        Buffer packet, first, second;
        fixture(id, source, packet);
        const auto &entry = *entries[id];
        auto group = core->openRoot();
        auto task = core->submit(group, entry,
                                 {entry.packetLayout, packet.data(), entry.packetBytes, &source});
        source.settle();
        require(task && core->drain() && core->failure(task).kind == TaskFailure::None,
                "generated entry execution failed");
        require(core->observe(task, *entry.result, observed, first.data()) &&
                    core->observe(task, *entry.result, observed, second.data()) &&
                    core->observe(task, *entry.result, survivor, second.data()),
                "generated entry observation failed");
        require(core->close(group) && core->drain() && core->release(group),
                "generated entry group cleanup failed");
        clean(*core);
        core.reset();
        verify(id, observed, first);
        verify(id, survivor, second);
        // Resubmit completed recursive data after the original scheduler is gone.
        if (id != 2) {
          auto onward = TaskCore::create(limits);
          require(bool(onward), "onward generated core creation failed");
          const auto &echo = *entries[id == 3 || id == 4 ? id : 0];
          auto nextGroup = onward->openRoot();
          auto nextTask = onward->submit(
              nextGroup, echo, {echo.packetLayout, second.data(), echo.packetBytes, &survivor});
          survivor.settle();
          Invocation final;
          Buffer result;
          require(nextTask && onward->drain() &&
                      onward->failure(nextTask).kind == TaskFailure::None &&
                      onward->observe(nextTask, *echo.result, final, result.data()),
                  "onward generated entry failed");
          require(onward->close(nextGroup) && onward->drain() && onward->release(nextGroup),
                  "onward group cleanup failed");
          clean(*onward);
          onward.reset();
          verify(id, final, result);
          final.settle();
          balanced(final);
        }
        observed.settle();
        survivor.settle();
        balanced(source);
        balanced(observed);
        balanced(survivor);
      }
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto core = TaskCore::create();
      require(bool(core), "invalid entry core creation failed");
      Invocation source, observed;
      Buffer packet, output;
      fixture(0, source, packet);
      auto &input = packet.get<Result<List>>();
      const auto &entry = *entries[mode == 3 ? 1 : 0];
      if (mode == 0)
        input.value.tail = reinterpret_cast<void *>(0x1230);
      if (mode == 1)
        input.value.tail = callable_box(nullptr, nullptr, 1, &source);
      if (mode == 2)
        input.value.tag = 0x100000002;
      if (mode == 3 || mode == 7)
        input.value = {1, 0, reinterpret_cast<void *>(0x1230)};
      if (mode == 4)
        source.settle();
      auto group = core->openRoot();
      auto task =
          core->submit(group, entry,
                       {mode == 5 ? entry.packetLayout + 1 : entry.packetLayout, packet.data(),
                        mode == 6 ? entry.packetBytes - 1 : entry.packetBytes, &source});
      source.settle();
      require(core->drain(), "invalid generated entry drain failed");
      if (mode == 7) {
        require(task && core->observe(task, *entry.result, observed, output.data()) &&
                    output.get<Result<List>>().value.tag == 1 && !observed.live(),
                "generated inactive payload retained backing");
      } else {
        const auto failure = task ? core->failure(task) : core->failure(group);
        require(failure.kind == (mode == 5 || mode == 6 ? TaskFailure::AdmissionRejected
                                                        : TaskFailure::Fault) &&
                    (mode == 5 || mode == 6 || failure.code == 3),
                "generated entry failure changed");
        if (task)
          require(!core->observe(task, *entry.result, observed, output.data()) &&
                      !output.get<Result<List>>().value.tag,
                  "failed generated entry published output");
      }
      require(core->close(group) && core->drain() && core->release(group),
              "invalid generated group cleanup failed");
      clean(*core);
      observed.settle();
      balanced(source);
      balanced(observed);
    }
    bool completed = false;
    for (unsigned attempt = 1; attempt < 40 && !completed; ++attempt) {
      Invocation source, destination;
      Buffer packet, frame;
      fixture(4, source, packet);
      const auto &entry = *entries[4];
      auto observer = source.lease(packet.get<Result<CallableTree>>().value.apply.environment);
      destination.failAllocationAt(attempt);
      completed = entry.construct(destination, frame.data(),
                                  {entry.packetLayout, packet.data(), entry.packetBytes, &source});
      if (completed)
        entry.destroy(frame.data());
      else
        require(destination.status() == 2, "generated constructor lost allocation failure");
      destination.settle();
      require(observer->referenceCount() == 2, "generated constructor partial retains leaked");
      source.settle();
      balanced(source);
      balanced(destination);
    }
    require(completed, "generated constructor allocation sweep never completed");
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
      auto core = TaskCore::create();
      require(bool(core), "allocation entry core creation failed");
      core->failAllocationAt(attempt);
      Invocation source;
      Buffer packet;
      fixture(0, source, packet);
      const auto &entry = *entries[0];
      auto group = core->openRoot();
      auto task = core->submit(group, entry,
                               {entry.packetLayout, packet.data(), entry.packetBytes, &source});
      source.settle();
      require(task && core->drain(), "generated frame allocation probe failed");
      require(core->failure(task).kind == (attempt < 3 ? TaskFailure::Fault : TaskFailure::None) &&
                  (attempt == 3 || core->failure(task).code == 2),
              "generated frame allocation status changed");
      require(core->close(group) && core->drain() && core->release(group),
              "allocation entry cleanup failed");
      clean(*core);
      balanced(source);
    }
    {
      TaskLimits limits;
      const auto &entry = *entries[0];
      limits.bytes = entry.frameBytes + entry.result->bytes - 1;
      auto core = TaskCore::create(limits);
      require(bool(core), "budget entry core creation failed");
      Invocation source;
      Buffer packet;
      fixture(0, source, packet);
      auto group = core->openRoot();
      auto task = core->submit(group, entry,
                               {entry.packetLayout, packet.data(), entry.packetBytes, &source});
      require(core->failure(group).kind == TaskFailure::AdmissionRejected &&
                  (!task || core->failure(task).kind == TaskFailure::AdmissionRejected),
              "generated frame budget bypassed");
      source.settle();
      require(core->close(group) && core->drain() && core->release(group),
              "budget entry cleanup failed");
      clean(*core);
      balanced(source);
    }
    for (unsigned workers : {1u, 2u, 4u})
      for (bool deadline : {false, true}) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "cancelled entry core creation failed");
        Invocation source;
        Buffer packet;
        fixture(0, source, packet);
        const auto &entry = *entries[6];
        auto group = core->openRoot();
        require(bool(core->submit(group, entry,
                                  {entry.packetLayout, packet.data(), entry.packetBytes, &source})),
                "cancelled entry submission failed");
        source.settle();
        require(core->cancel(group, deadline) && core->drain(),
                "generated entry cancellation failed");
        require(core->failure(group).kind ==
                    (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
                "generated cancellation variant changed");
        require(core->close(group) && core->drain() && core->release(group),
                "cancelled entry cleanup failed");
        clean(*core);
        balanced(source);
      }
    std::cout << "recursive generated task entries passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
