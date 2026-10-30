#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct List {
  std::int64_t tag, head;
  const void *tail;
};
using Code = std::int64_t (*)(const void *, std::int64_t, Invocation *);
struct Closure {
  Code code;
  const void *environment;
};
struct Captures {
  StringDescriptor text;
  List list;
};
struct Tree {
  std::int64_t tag;
  Closure apply;
  StringArrayDescriptor items, children;
  List list;
};
struct Error {
  std::int64_t tag;
  StringDescriptor message;
  List node;
  Tree work;
};
template <class T> struct Result {
  Failure failure;
  T value;
};
struct Outcome {
  Failure failure;
  List value;
  Error error;
};
struct Buffer {
  alignas(std::max_align_t) std::array<std::byte, 4096> bytes{};
  void *data() { return bytes.data(); }
};
static_assert(sizeof(Error) == 184 && sizeof(Outcome) == 240 && sizeof(Result<Error>) == 216);
extern "C" {
const void *make_nested_list(Invocation *);
bool task_project_tail(Invocation *, const List *, List *);
std::int64_t kalandra_callable_0(const void *, std::int64_t, Invocation *);
extern const TaskEntry *recursive_generated_entry_12;
extern const TaskEntry *recursive_generated_entry_13;
extern const TaskEntry *recursive_generated_entry_14;
extern const TaskEntry *recursive_generated_entry_15;
extern const TaskEntry *recursive_generated_entry_16;
extern const TaskEntry *recursive_generated_entry_17;
extern const TaskEntry *recursive_generated_entry_18;
extern const TaskEntry *recursive_generated_entry_19;
extern const TaskEntry *recursive_generated_entry_20;
extern const TaskEntry *recursive_generated_entry_21;
extern const TaskEntry *recursive_generated_entry_22;
extern const TaskEntry *recursive_generated_entry_23;
extern const TaskEntry *recursive_generated_entry_24;
extern const TaskEntry *recursive_generated_entry_25;
extern const TaskEntry *recursive_generated_entry_26;
extern const TaskEntry *recursive_generated_entry_27;
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "recursive error invocation leaked");
}
static void clean(const TaskCore &core) {
  auto stats = core.stats();
  require(!stats.retainedOwners && !stats.retainedBytes && !stats.environmentBytes &&
              !stats.records && !stats.groups && !stats.unfinished && !stats.pending &&
              !stats.externalPending,
          "recursive error scheduler leaked");
}
static List list(Invocation &source) {
  auto lease = source.lease(make_nested_list(&source));
  require(bool(lease), "recursive error list fixture missing");
  List value;
  std::memcpy(&value, lease->bytes().data(), sizeof(value));
  return value;
}
static Error fixture(Invocation &source) {
  static const char key = 0;
  const auto node = list(source);
  StringDescriptor text;
  kalandra_string_literal_v1(&source, &text, "é", 2);
  Captures captures{text, node};
  const void *dependencies[]{text.allocated, node.tail};
  auto environment = source.makeRecursiveValueFromPointers(std::as_bytes(std::span{&captures, 1}),
                                                           &key, dependencies);
  require(environment && source.bindRecursiveEnvironment(
                             environment, reinterpret_cast<const void *>(kalandra_callable_0)),
          "recursive error environment fixture missing");
  const Closure apply{kalandra_callable_0, environment};
  auto *functions = static_cast<Closure *>(source.allocateRecord(2 * sizeof(Closure)));
  auto *child = static_cast<Tree *>(source.allocateRecord(sizeof(Tree)));
  require(functions && child, "recursive error array allocation failed");
  functions[0] = functions[1] = apply;
  *child = {2, apply, {functions, functions, 0, 2, 1}, {}, node};
  return {
      1, text, node, {2, apply, {functions, functions, 0, 2, 1}, {child, child, 0, 1, 1}, node}};
}
static void verifyList(Invocation &context, const List &node) {
  List child{};
  require(node.tag == 2 && node.head == 10 && task_project_tail(&context, &node, &child) &&
              child.tag == 2 && child.head == 42,
          "recursive error list backing lost");
}
static void verify(Invocation &context, const Error &error) {
  require(error.tag == 1 && error.message.byteLength == 2 &&
              !std::memcmp(error.message.data, "é", 2),
          "recursive error tag or text changed");
  verifyList(context, error.node);
  const auto checkTree = [&](const Tree &tree) {
    require(tree.tag == 2 && tree.apply.code(tree.apply.environment, 3, &context) == 47 &&
                tree.items.size == 2,
            "recursive error callable capture lost");
    const auto *items = static_cast<const Closure *>(tree.items.data);
    require(items[0].environment == items[1].environment,
            "recursive error duplicate environments lost identity");
    for (unsigned i = 0; i < 2; ++i)
      require(items[i].code(items[i].environment, 3, &context) == 47,
              "recursive error function array lost captures");
    verifyList(context, tree.list);
  };
  checkTree(error.work);
  require(error.work.children.size == 1, "recursive error child array lost");
  checkTree(*static_cast<const Tree *>(error.work.children.data));
  require(!context.status(), "recursive error inspection faulted");
}
struct PlainError {
  std::int64_t tag;
  StringDescriptor message;
  List node;
};
struct PlainOutcome {
  Failure failure;
  List value;
  PlainError error;
};
static void copyChecks() {
  const auto &layout = *recursive_generated_entry_15->result;
  require(layout.bytes == sizeof(Outcome), "recursive error result ABI changed");
  for (unsigned mode = 0; mode < 4; ++mode) {
    Invocation source, destination;
    Outcome input{{}, {}, fixture(source)}, output{};
    if (mode == 0) {
      input.value = {0, 0, reinterpret_cast<const void *>(0x1234)};
    } else if (mode == 1) {
      input.value = list(source);
      input.error.tag = 0;
      input.error.node.tail = reinterpret_cast<const void *>(0x1234);
      input.error.work.apply.environment = reinterpret_cast<const void *>(0x1234);
    } else if (mode == 2) {
      input.failure = {TaskFailure::Fault, 91, 7, 11};
      input.error.tag = -1;
      input.value.tail = reinterpret_cast<const void *>(0x1234);
    } else {
      input.error.tag = 2;
      input.error.node.tail = reinterpret_cast<const void *>(0x1234);
      input.error.work.apply.environment = reinterpret_cast<const void *>(0x1234);
    }
    const auto original = input;
    require(layout.copy(destination, &output, source, &input),
            "active-channel recursive error copy failed");
    require(!std::memcmp(&original, &input, sizeof(input)),
            "recursive error copy mutated its input");
    source.settle();
    if (mode == 0)
      verify(destination, output.error);
    else if (mode == 1)
      verifyList(destination, output.value);
    else if (mode == 2)
      require(output.failure.kind == TaskFailure::Fault && output.failure.code == 91 &&
                  output.failure.line == 7 && output.failure.column == 11 && !destination.live(),
              "execution failure did not bypass inactive payloads");
    else
      require(output.error.tag == 2 && !destination.live(), "payload-free error retained garbage");
    destination.settle();
    balanced(source);
    balanced(destination);
  }
  for (unsigned mode = 0; mode < 6; ++mode) {
    Invocation source, destination;
    Outcome input{{}, {}, fixture(source)};
    if (mode == 0)
      input.error.tag = (std::int64_t{1} << 32) | 1;
    else if (mode == 1)
      input.error.node.tail = reinterpret_cast<const void *>(0x1234);
    else if (mode == 2)
      input.error.work.apply.environment = reinterpret_cast<const void *>(0x1234);
    else if (mode == 3)
      input.error.work.children.data = reinterpret_cast<void *>(0x1234);
    else if (mode == 4)
      input.error.tag = 3;
    else
      source.settle();
    alignas(Outcome) std::array<std::byte, sizeof(Outcome)> output;
    output.fill(std::byte{0x5a});
    const auto original = output;
    require(!layout.copy(destination, output.data(), source, &input) && output == original,
            "malformed recursive error result was published");
    destination.settle();
    source.settle();
    balanced(destination);
    balanced(source);
  }
  bool succeeded = false;
  unsigned failures = 0;
  for (unsigned attempt = 1; attempt < 128 && !succeeded; ++attempt) {
    Invocation source, destination;
    Outcome input{{}, {}, fixture(source)};
    alignas(Outcome) std::array<std::byte, sizeof(Outcome)> output;
    output.fill(std::byte{0x5a});
    const auto original = output;
    destination.failAllocationAt(attempt);
    if (layout.copy(destination, output.data(), source, &input)) {
      succeeded = true;
      source.settle();
      verify(destination, reinterpret_cast<const Outcome *>(output.data())->error);
    } else {
      ++failures;
      require(destination.status() == 2 && output == original,
              "failed recursive error copy published a partial outcome");
    }
    destination.settle();
    source.settle();
    balanced(destination);
    balanced(source);
  }
  require(succeeded && failures > 2, "recursive error allocation sweep missed ownership stages");
  {
    Invocation source, destination;
    const auto owned = fixture(source);
    PlainOutcome input{{}, {}, {1, owned.message, owned.node}}, output{};
    const auto &plainLayout = *recursive_generated_entry_18->result;
    require(plainLayout.bytes == sizeof(PlainOutcome) &&
                plainLayout.copy(destination, &output, source, &input),
            "plain recursive error ownership copy failed");
    source.settle();
    require(output.error.tag == 1 && output.error.message.byteLength == 2 &&
                !std::memcmp(output.error.message.data, "é", 2),
            "plain recursive error String backing lost");
    verifyList(destination, output.error.node);
    destination.settle();
    balanced(source);
    balanced(destination);
  }
  for (bool plain : {false, true})
    for (unsigned mode = 0; mode < 4; ++mode) {
      Invocation source, destination;
      Outcome input{{}, {}, fixture(source)};
      PlainOutcome plainInput{{}, {}, {2, {}, {}}};
      alignas(Outcome) std::array<std::byte, sizeof(Outcome)> output;
      output.fill(std::byte{0x5a});
      const auto original = output;
      if (mode == 0)
        destination.expectedTaskError(1, 81, 2, 3);
      else if (mode == 1)
        destination.expectedApplicationError("Existing", "Stopped");
      else if (mode == 2)
        destination.fault(93);
      else
        destination.settle();
      const auto &copy = plain ? *recursive_generated_entry_18->result : layout;
      require(!copy.copy(destination, output.data(), source,
                         plain ? static_cast<void *>(&plainInput) : static_cast<void *>(&input)) &&
                  output == original,
              "stopped destination accepted a recursive error result");
      require(mode == 0   ? destination.hasTaskError() && !destination.status()
              : mode == 1 ? destination.hasApplicationError() && !destination.status()
                          : destination.status() == (mode == 2 ? 93 : 3),
              "recursive error copy replaced a pending error");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
}
struct SmallError {
  Error application;
  std::array<Failure, 4> tasks;
};
struct WideError {
  Error application;
  StringDescriptor plainMessage;
  List plainNode;
  std::array<Failure, 4> tasks;
};
struct SmallOutcome {
  Failure failure;
  List value;
  SmallError error;
};
struct WideOutcome {
  Failure failure;
  List value;
  WideError error;
};
static_assert(sizeof(SmallError) == 312 && sizeof(WideError) == 360 &&
              sizeof(SmallOutcome) == 368 && sizeof(WideOutcome) == 416);
static void runMixed(const TaskEntry &entry, Invocation &source, const void *input,
                     std::size_t bytes, Invocation &destination, void *output, unsigned workers,
                     bool valid = true) {
  require(bytes == entry.packetBytes, "mixed error packet ABI changed");
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  require(bool(core), "mixed error core creation failed");
  const auto group = core->openRoot();
  const auto task = core->submit(group, entry, {entry.packetLayout, input, bytes, &source});
  source.settle();
  require(task && core->drain(), "mixed error task submission or drain failed");
  if (valid) {
    Invocation independent;
    Buffer repeated;
    require(core->failure(task).kind == TaskFailure::None &&
                core->observe(task, *entry.result, destination, output) &&
                core->observe(task, *entry.result, destination, repeated.data()) &&
                core->observe(task, *entry.result, independent, repeated.data()),
            "mixed error observation failed");
    independent.settle();
    balanced(independent);
  } else
    require(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 3 &&
                !core->observe(task, *entry.result, destination, output),
            "invalid mixed error narrowing was admitted");
  require(core->close(group) && core->drain() && core->release(group),
          "mixed error group cleanup failed");
  clean(*core);
  core.reset();
  balanced(source);
}
static void sameFailure(const Failure &a, const Failure &b) {
  require(a.kind == b.kind && a.code == b.code && a.line == b.line && a.column == b.column,
          "mixed error lost TaskError details");
}
static void mixedChecks() {
  for (unsigned workers : {1u, 2u, 4u}) {
    for (unsigned target = 0; target < 3; ++target) {
      Invocation source, smallOwner, wideOwner, final;
      Outcome input{{}, {}, fixture(source)};
      SmallOutcome small{};
      WideOutcome wide{};
      runMixed(*recursive_generated_entry_19, source, &input, sizeof(input), smallOwner, &small,
               workers);
      verify(smallOwner, small.error.application);
      runMixed(*recursive_generated_entry_20, smallOwner, &small, sizeof(small), wideOwner, &wide,
               workers);
      verify(wideOwner, wide.error.application);
      if (target == 0) {
        Result<WideError> packet{{}, wide.error};
        Result<Error> result{};
        runMixed(*recursive_generated_entry_21, wideOwner, &packet, sizeof(packet), final, &result,
                 workers);
        verify(final, result.value);
      } else if (target == 1) {
        Result<WideError> packet{{}, wide.error};
        Result<SmallError> result{};
        runMixed(*recursive_generated_entry_22, wideOwner, &packet, sizeof(packet), final, &result,
                 workers);
        verify(final, result.value.application);
      } else {
        Result<List> result{};
        runMixed(*recursive_generated_entry_25, wideOwner, &wide, sizeof(wide), final, &result,
                 workers);
        verifyList(final, result.value);
      }
      final.settle();
      balanced(final);
    }
    for (unsigned kind = 1; kind <= 4; ++kind)
      for (unsigned route = 0; route < 3; ++route) {
        Invocation source, smallOwner, wideOwner, final;
        const Failure expected{static_cast<TaskFailure>(kind), 70 + kind, 100 + kind, 200 + kind};
        WideOutcome wide{};
        if (route == 0) {
          Outcome input{};
          input.failure = expected;
          input.error.tag = -1;
          input.error.node.tail = reinterpret_cast<const void *>(0x1234);
          SmallOutcome small{};
          runMixed(*recursive_generated_entry_19, source, &input, sizeof(input), smallOwner, &small,
                   workers);
          require(small.failure.kind == TaskFailure::None &&
                      small.error.application.tag == kind + 2,
                  "TaskError did not normalize into smaller union");
          sameFailure(small.error.tasks[kind - 1], expected);
          runMixed(*recursive_generated_entry_20, smallOwner, &small, sizeof(small), wideOwner,
                   &wide, workers);
        } else if (route == 1) {
          Result<Failure> packet{{}, expected};
          runMixed(*recursive_generated_entry_27, source, &packet, sizeof(packet), wideOwner, &wide,
                   workers);
        } else {
          Outcome input{};
          input.failure = expected;
          input.error.tag = -1;
          runMixed(*recursive_generated_entry_26, source, &input, sizeof(input), wideOwner, &wide,
                   workers);
        }
        require(wide.failure.kind == TaskFailure::None && wide.error.application.tag == kind + 4,
                "mixed group normalization left a failure header or wrong variant");
        sameFailure(wide.error.tasks[kind - 1], expected);
        Result<WideError> packet{{}, wide.error};
        if (route == 0) {
          Result<SmallError> result{};
          runMixed(*recursive_generated_entry_22, wideOwner, &packet, sizeof(packet), final,
                   &result, workers);
          require(result.value.application.tag == kind + 2,
                  "residual union narrowing did not remap TaskError variant");
          sameFailure(result.value.tasks[kind - 1], expected);
        } else {
          Result<Failure> result{};
          runMixed(*recursive_generated_entry_23, wideOwner, &packet, sizeof(packet), final,
                   &result, workers);
          sameFailure(result.value, expected);
        }
        final.settle();
        smallOwner.settle();
        balanced(final);
        balanced(smallOwner);
      }
    {
      Invocation source, intermediate, final;
      const auto data = fixture(source);
      WideOutcome input{};
      input.error.application.tag = 3;
      input.error.application.node.tail = reinterpret_cast<const void *>(0x1234);
      input.error.application.work.apply.environment = reinterpret_cast<const void *>(0x1234);
      input.error.plainMessage = data.message;
      input.error.plainNode = data.node;
      WideOutcome echoed{};
      runMixed(*recursive_generated_entry_24, source, &input, sizeof(input), intermediate, &echoed,
               workers);
      require(echoed.error.application.tag == 3 && echoed.error.plainMessage.byteLength == 2 &&
                  !std::memcmp(echoed.error.plainMessage.data, "é", 2),
              "second application family lost its owned payload");
      Result<List> result{};
      runMixed(*recursive_generated_entry_25, intermediate, &echoed, sizeof(echoed), final, &result,
               workers);
      verifyList(final, result.value);
      final.settle();
      balanced(final);
    }
    {
      Invocation source, smallOwner, wideOwner, final;
      Outcome input{};
      input.value = list(source);
      input.error.node.tail = reinterpret_cast<const void *>(0x1234);
      SmallOutcome small{};
      WideOutcome wide{};
      runMixed(*recursive_generated_entry_19, source, &input, sizeof(input), smallOwner, &small,
               workers);
      runMixed(*recursive_generated_entry_20, smallOwner, &small, sizeof(small), wideOwner, &wide,
               workers);
      require(!wide.error.application.tag, "successful widening fabricated an application error");
      Result<List> result{};
      runMixed(*recursive_generated_entry_25, wideOwner, &wide, sizeof(wide), final, &result,
               workers);
      verifyList(final, result.value);
      final.settle();
      balanced(final);
    }
  }
  for (unsigned mode = 0; mode < 3; ++mode) {
    Invocation source, destination;
    Result<WideError> input{};
    const auto data = fixture(source);
    input.value.application = data;
    if (mode != 2) {
      input.value.application.tag = 3;
      input.value.plainMessage = data.message;
      input.value.plainNode = data.node;
    }
    Buffer output;
    output.bytes.fill(std::byte{0x5a});
    const auto original = output.bytes;
    const auto &entry = *(mode == 0   ? recursive_generated_entry_21
                          : mode == 1 ? recursive_generated_entry_22
                                      : recursive_generated_entry_23);
    runMixed(entry, source, &input, sizeof(input), destination, output.data(), 2, false);
    require(output.bytes == original, "invalid narrowed result was published");
    destination.settle();
    balanced(destination);
  }
  const auto &layout = *recursive_generated_entry_24->result;
  for (unsigned mode = 0; mode < 4; ++mode) {
    Invocation source, destination;
    WideOutcome input{};
    input.error.application.tag = mode == 0 ? (std::int64_t{1} << 32) | 5 : 5;
    input.error.tasks[0] = {mode == 1 ? TaskFailure::Cancelled : TaskFailure::Fault, 4, 9, 11};
    if (mode == 2)
      input.error.application.tag = 9;
    if (mode == 3)
      input.error.tasks[0].kind = static_cast<TaskFailure>(5);
    Buffer output;
    output.bytes.fill(std::byte{0x5a});
    const auto original = output.bytes;
    require(!layout.copy(destination, output.data(), source, &input) && output.bytes == original,
            "malformed mixed error variant was published");
    source.settle();
    destination.settle();
    balanced(source);
    balanced(destination);
  }
}
int main() {
  try {
    copyChecks();
    mixedChecks();
    const std::array entries{recursive_generated_entry_12, recursive_generated_entry_13,
                             recursive_generated_entry_14};
    for (unsigned workers : {1u, 2u, 4u})
      for (unsigned id = 0; id < entries.size(); ++id) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "recursive error core missing");
        Invocation source, observed, survivor;
        Result<Error> packet{{}, fixture(source)};
        Buffer first, second;
        const auto &entry = *entries[id];
        auto group = core->openRoot();
        auto task =
            core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet), &source});
        source.settle();
        require(task && core->drain() && core->failure(task).kind == TaskFailure::None &&
                    core->observe(task, *entry.result, observed, first.data()) &&
                    core->observe(task, *entry.result, observed, second.data()) &&
                    core->observe(task, *entry.result, survivor, second.data()),
                "recursive error propagation failed");
        require(core->close(group) && core->drain() && core->release(group),
                "recursive error group cleanup failed");
        clean(*core);
        core.reset();
        if (id == 2) {
          verifyList(observed, reinterpret_cast<Result<List> *>(first.data())->value);
          verifyList(survivor, reinterpret_cast<Result<List> *>(second.data())->value);
        } else {
          auto *a = reinterpret_cast<Outcome *>(first.data());
          auto *b = reinterpret_cast<Outcome *>(second.data());
          require(a->failure.kind == TaskFailure::None && b->failure.kind == TaskFailure::None,
                  "application error became an execution fault");
          verify(observed, a->error);
          verify(survivor, b->error);
          // Resubmit the cached application outcome after the producing core
          // has gone, then recover the recursive payload in a generated task.
          auto onward = TaskCore::create(limits);
          require(bool(onward), "recursive error onward core missing");
          const auto &echo = *recursive_generated_entry_15;
          auto nextGroup = onward->openRoot();
          auto nextTask =
              onward->submit(nextGroup, echo, {echo.packetLayout, b, sizeof(*b), &survivor});
          survivor.settle();
          Invocation final;
          Outcome result{};
          require(nextTask && onward->drain() &&
                      onward->failure(nextTask).kind == TaskFailure::None &&
                      onward->observe(nextTask, *echo.result, final, &result),
                  "recursive error onward observation failed");
          require(onward->close(nextGroup) && onward->drain() && onward->release(nextGroup),
                  "recursive error onward cleanup failed");
          clean(*onward);
          onward.reset();
          verify(final, result.error);
          auto recovery = TaskCore::create(limits);
          require(bool(recovery), "recursive error recovery core missing");
          const auto &recover = *recursive_generated_entry_17;
          auto recoverGroup = recovery->openRoot();
          auto recoverTask = recovery->submit(
              recoverGroup, recover, {recover.packetLayout, &result, sizeof(result), &final});
          final.settle();
          Invocation recovered;
          Result<List> recoveredValue{};
          require(recoverTask && recovery->drain() &&
                      recovery->failure(recoverTask).kind == TaskFailure::None &&
                      recovery->observe(recoverTask, *recover.result, recovered, &recoveredValue),
                  "recursive error recovery failed");
          require(recovery->close(recoverGroup) && recovery->drain() &&
                      recovery->release(recoverGroup),
                  "recursive error recovery cleanup failed");
          clean(*recovery);
          recovery.reset();
          verifyList(recovered, recoveredValue.value);
          recovered.settle();
          balanced(recovered);
          balanced(final);
        }
        observed.settle();
        survivor.settle();
        balanced(source);
        balanced(observed);
        balanced(survivor);
      }
    for (unsigned workers : {1u, 2u, 4u})
      for (bool deadline : {false, true}) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "cancelled recursive error core missing");
        Invocation source;
        Result<Error> input{{}, fixture(source)};
        const auto &entry = *recursive_generated_entry_13;
        auto group = core->openRoot();
        require(
            bool(core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source})),
            "cancelled recursive error submission failed");
        source.settle();
        require(core->cancel(group, deadline) && core->drain() &&
                    core->failure(group).kind ==
                        (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
                "recursive error cancellation status changed");
        require(core->close(group) && core->drain() && core->release(group),
                "cancelled recursive error cleanup failed");
        clean(*core);
        balanced(source);
      }
    std::cout << "recursive task application errors passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
