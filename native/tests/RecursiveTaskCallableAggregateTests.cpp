#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <atomic>
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
struct FunctionRecord {
  std::int64_t tag;
  StringArrayDescriptor items;
};
struct Tree {
  std::int64_t tag;
  Closure apply;
  StringArrayDescriptor items, children;
  List list;
};
template <class T> struct Result {
  Failure failure;
  T value;
};
static_assert(sizeof(Tree) == 128 && sizeof(Result<Tree>) == 160 &&
              sizeof(Result<FunctionRecord>) == 80 && sizeof(Result<StringArrayDescriptor>) == 72);
extern "C" {
const void *make_nested_list(Invocation *);
bool task_project_tail(Invocation *, const List *, List *);
std::int64_t kalandra_callable_0(const void *, std::int64_t, Invocation *);
std::int64_t kalandra_callable_1(const void *, std::int64_t, Invocation *);
bool task_copy_functions(Invocation *, void *, const Invocation *, const void *);
bool task_copy_restricted_functions(Invocation *, void *, const Invocation *, const void *);
bool task_copy_function_record(Invocation *, void *, const Invocation *, const void *);
bool task_copy_callable_tree(Invocation *, void *, const Invocation *, const void *);
bool task_copy_callable_tree_array(Invocation *, void *, const Invocation *, const void *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static const void *code(Code value) { return reinterpret_cast<const void *>(value); }
static List list(Invocation &context) {
  auto retained = context.lease(make_nested_list(&context));
  require(bool(retained), "aggregate List fixture missing");
  List value;
  std::memcpy(&value, retained->bytes().data(), sizeof(value));
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
  require(environment && context.bindRecursiveEnvironment(environment, code(kalandra_callable_0)),
          "aggregate environment fixture failed");
  return {kalandra_callable_0, environment};
}
static StringArrayDescriptor functions(Invocation &context) {
  const auto captured = closure(context);
  const Closure free{kalandra_callable_1, nullptr};
  auto *cells = static_cast<Closure *>(context.allocateRecord(4 * sizeof(Closure)));
  require(cells, "aggregate function array fixture failed");
  cells[0] = cells[1] = captured;
  cells[2] = cells[3] = free;
  return {cells, cells, 0, 4, 1};
}
static Tree tree(Invocation &context) {
  const auto items = functions(context);
  auto *child = static_cast<Tree *>(context.allocateRecord(sizeof(Tree)));
  require(child, "aggregate child allocation failed");
  auto values = list(context);
  *child = {2, {kalandra_callable_1, nullptr}, items, {}, values};
  return {2, static_cast<Closure *>(items.data)[0], items, {child, child, 0, 1, 1}, values};
}
static void verifyClosure(Invocation &context, const Closure &value) {
  require(value.code == kalandra_callable_0 || value.code == kalandra_callable_1,
          "unknown callable returned");
  const auto expected = value.code == kalandra_callable_0 ? 54 : 20;
  require(value.code(value.environment, 10, &context) == expected && !context.status(),
          "aggregate callable lost captured values");
  if (value.code == kalandra_callable_0) {
    const auto *captures = static_cast<const Captures *>(
        context.readRecursiveEnvironment(value.environment, sizeof(Captures), code(value.code)));
    require(captures && !std::memcmp(captures->text.data, "é", 2), "captured String bytes lost");
  } else
    require(!value.environment, "capture-free environment changed");
}
static void verifyFunctions(Invocation &context, const StringArrayDescriptor &items) {
  if (!items.size) {
    require(!items.data && !items.allocated, "empty array changed");
    return;
  }
  require(items.size == 4 && items.stride == 1, "function array shape changed");
  const auto *cells = static_cast<const Closure *>(items.data);
  require(cells[0].environment == cells[1].environment, "duplicate environments were cloned");
  for (unsigned i = 0; i < 4; ++i)
    verifyClosure(context, cells[i]);
}
static void verifyTree(Invocation &context, const Tree &value, unsigned depth = 0) {
  require(depth < 8, "unexpected fixture tree depth");
  if (value.tag == 1)
    return;
  require(value.tag == 2, "tree discriminator changed");
  verifyClosure(context, value.apply);
  verifyFunctions(context, value.items);
  List child{};
  require(task_project_tail(&context, &value.list, &child) && child.head == 42,
          "aggregate recursive List child lost");
  for (std::int64_t i = 0; i < value.children.size; ++i)
    verifyTree(context, static_cast<const Tree *>(value.children.data)[i], depth + 1);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "aggregate owners or checked call entries leaked");
}
static bool copyTree(Invocation &destination, void *output, const Invocation &source,
                     const void *input) noexcept {
  return task_copy_callable_tree(&destination, output, &source, input);
}
static void nothing(void *) noexcept {}
static const ValueLayout layout{86001, sizeof(Result<Tree>), alignof(Result<Tree>), copyTree,
                                nothing};
static bool construct(Invocation &destination, void *frame, const Packet &packet) {
  return packet.source && copyTree(destination, frame, *packet.source, packet.data);
}
static void resume(Task &task) {
  verifyTree(task.context(), static_cast<const Result<Tree> *>(task.frame())->value);
  task.complete(task.frame());
}
static const TaskEntry entry{1,
                             86002,
                             sizeof(Result<Tree>),
                             alignof(Result<Tree>),
                             sizeof(Result<Tree>),
                             alignof(Result<Tree>),
                             construct,
                             nothing,
                             resume,
                             &layout};
int main() {
  try {
    for (unsigned mode = 0; mode < 16; ++mode) {
      Invocation source, destination, foreign;
      Result<StringArrayDescriptor> input{{}, functions(source)}, output{};
      auto *cells = static_cast<Closure *>(input.value.data);
      if (mode == 1 || mode == 14)
        input.value = {};
      if (mode == 2)
        cells[0].code = reinterpret_cast<Code>(0x1230);
      if (mode == 3)
        cells[0].environment = closure(foreign).environment;
      if (mode == 4)
        cells[0].environment = source.allocateEnvironment(sizeof(Captures), code(cells[0].code));
      if (mode == 5)
        input.value.stride = 2;
      if (mode == 6)
        input.value.allocated = input.value.data = reinterpret_cast<void *>(0x1230);
      if (mode == 7)
        input.value.size = INT64_MAX;
      if (mode == 15)
        cells[3].code = nullptr;
      Closure original[4];
      std::memcpy(original, cells, sizeof(original));
      std::atomic<bool> cancelled{mode == 8};
      destination.bindCancellation(&cancelled);
      if (mode == 9)
        destination.fault(5);
      if (mode == 10 || mode == 14)
        destination.expectedTaskError(1, 2, 0, 0);
      if (mode == 11)
        destination.expectedApplicationError("Problem", "Invalid");
      if (mode == 12)
        destination.settle();
      if (mode == 13)
        source.settle();
      const bool ok = task_copy_functions(&destination, &output, &source, &input);
      require(ok == (mode < 2), "invalid function array accepted");
      if (ok) {
        require(!input.value.size || output.value.data != input.value.data,
                "mutable transport scratch aliases source array");
        require(!std::memcmp(original, cells, sizeof(original)), "source closure cells changed");
        source.settle();
        verifyFunctions(destination, output.value);
      } else {
        require(!output.value.data && !output.value.size, "failed function array published output");
        if (mode == 9)
          require(destination.status() == 5, "aggregate existing fault replaced");
        if (mode == 10 || mode == 11 || mode == 14)
          require(destination.hasExpectedError() && !destination.status(),
                  "aggregate expected error replaced");
      }
      source.settle();
      destination.settle();
      foreign.settle();
      balanced(source);
      balanced(destination);
      balanced(foreign);
    }
    // A separately emitted selector must not reuse earlier, wider callable admission.
    {
      Invocation source, destination;
      Result<StringArrayDescriptor> input{{}, functions(source)}, output{};
      require(!task_copy_restricted_functions(&destination, &output, &source, &input) &&
                  destination.status() == 3 && !output.value.data,
              "restricted selector accepted captured entry");
      destination.settle();
      auto *cells = static_cast<Closure *>(input.value.data);
      for (unsigned i = 0; i < 4; ++i)
        cells[i] = {kalandra_callable_1, nullptr};
      Invocation accepted;
      require(task_copy_restricted_functions(&accepted, &output, &source, &input),
              "restricted selector rejected known free entry");
      source.settle();
      verifyFunctions(accepted, output.value);
      accepted.settle();
      balanced(source);
      balanced(destination);
      balanced(accepted);
    }
    for (unsigned mode = 0; mode < 5; ++mode) {
      Invocation source, destination;
      Result<FunctionRecord> input{{}, {2, functions(source)}}, output{};
      if (mode == 1)
        input.value = {1, {reinterpret_cast<void *>(0x1230), nullptr, -1, -1, -1}};
      if (mode >= 2)
        input.value.tag = mode == 2 ? 0 : mode == 3 ? -1 : 0x100000002;
      const bool ok = task_copy_function_record(&destination, &output, &source, &input);
      require(ok == (mode < 2), "function record tag validation failed");
      if (ok && !mode) {
        source.settle();
        verifyFunctions(destination, output.value.items);
      }
      if (mode == 1)
        require(!destination.live(), "inactive function array retained invalid backing");
      if (mode >= 2)
        require(destination.status() == 3 && !output.value.tag,
                "invalid tag did not stop publication");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    bool succeeded = false;
    for (unsigned attempt = 1; attempt < 60 && !succeeded; ++attempt) {
      Invocation source, destination;
      Result<Tree> input{{}, tree(source)}, output{};
      auto observer = source.lease(input.value.apply.environment);
      Tree before = input.value;
      destination.failAllocationAt(attempt);
      succeeded = task_copy_callable_tree(&destination, &output, &source, &input);
      require(!std::memcmp(&before, &input.value, sizeof(Tree)), "source aggregate fields changed");
      if (succeeded) {
        source.settle();
        verifyTree(destination, output.value);
      } else
        require(destination.status() == 2 && !output.value.tag,
                "aggregate allocation failure published output");
      destination.settle();
      source.settle();
      require(observer->referenceCount() == 1, "partial aggregate retain survived cleanup");
      balanced(source);
      balanced(destination);
    }
    require(succeeded, "aggregate allocation sweep never completed");
    for (unsigned mode = 0; mode < 3; ++mode) {
      Invocation source, destination;
      auto *cells = static_cast<Tree *>(source.allocateRecord(2 * sizeof(Tree)));
      require(cells, "callable tree array allocation failed");
      cells[0] = tree(source);
      cells[1] = {1, {reinterpret_cast<Code>(0x1230), nullptr}, {}, {}, {}};
      if (mode == 1)
        cells[0].tag = 0;
      if (mode == 2)
        cells[0].apply.code = nullptr;
      Result<StringArrayDescriptor> input{{}, {cells, cells, 0, 2, 1}}, output{};
      require(task_copy_callable_tree_array(&destination, &output, &source, &input) == (mode == 0),
              "callable tree array copy failed");
      if (!mode) {
        source.settle();
        auto *values = static_cast<const Tree *>(output.value.data);
        verifyTree(destination, values[0]);
        verifyTree(destination, values[1]);
      } else
        require(destination.status() == 3 && !output.value.data, "invalid tree array published");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    // Array-mediated callable type recursion stays finite during emission and bounded during
    // copying.
    for (unsigned depth : {4u, 160u}) {
      Invocation source, destination;
      StringArrayDescriptor children{};
      Tree current{};
      for (unsigned i = 0; i < depth; ++i) {
        current = {2,
                   {kalandra_callable_1, nullptr},
                   {},
                   children,
                   {1, 0, reinterpret_cast<void *>(0x1230)}};
        auto *cell = static_cast<Tree *>(source.allocateRecord(sizeof(Tree)));
        require(cell, "deep callable tree allocation");
        *cell = current;
        children = {cell, cell, 0, 1, 1};
      }
      Result<Tree> input{{}, current}, output{};
      const bool ok = task_copy_callable_tree(&destination, &output, &source, &input);
      require(ok && !destination.status(), "valid deep callable tree retained a call cap");
      if (!ok)
        require(destination.status() == 6 && !output.value.tag,
                "deep callable aggregate fault changed");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    for (unsigned workers : {1u, 2u, 4u})
      for (unsigned mode = 0; mode < 3; ++mode) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "callable aggregate core creation failed");
        Invocation source, observed, survivor;
        Result<Tree> input{{}, tree(source)}, first{}, second{};
        auto group = core->openRoot();
        auto task =
            core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
        source.settle();
        if (mode)
          require(core->cancel(group, mode == 2), "aggregate cancellation failed");
        require(task && core->drain(), "aggregate drain failed");
        if (!mode) {
          require(core->failure(task).kind == TaskFailure::None &&
                      core->observe(task, layout, observed, &first) &&
                      core->observe(task, layout, observed, &second) &&
                      core->observe(task, layout, survivor, &second),
                  "aggregate repeated/independent observation failed");
        } else
          require(core->failure(group).kind ==
                      (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
                  "aggregate cancellation status changed");
        require(core->close(group) && core->drain() && core->release(group),
                "aggregate group cleanup failed");
        const auto stats = core->stats();
        require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups,
                "aggregate scheduler leaked");
        core.reset();
        if (!mode) {
          verifyTree(observed, first.value);
          verifyTree(survivor, second.value);
          Invocation onward;
          Result<Tree> third{};
          require(task_copy_callable_tree(&onward, &third, &survivor, &second),
                  "onward aggregate handoff failed");
          survivor.settle();
          observed.settle();
          verifyTree(onward, third.value);
          onward.settle();
          balanced(onward);
        }
        observed.settle();
        survivor.settle();
        balanced(source);
        balanced(observed);
        balanced(survivor);
      }
    std::cout << "recursive task callable aggregates passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
