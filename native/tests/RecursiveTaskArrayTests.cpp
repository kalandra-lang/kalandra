#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Tree {
  StringDescriptor name;
  StringArrayDescriptor children;
};
struct List {
  std::int64_t tag, head;
  const void *tail;
};
struct ArrayValue {
  Failure failure;
  StringArrayDescriptor items;
};
static_assert(sizeof(Tree) == 64 && sizeof(ArrayValue) == 72);
extern "C" {
bool task_copy_tree_array(Invocation *, ArrayValue *, const Invocation *, const ArrayValue *);
bool task_copy_list_array(Invocation *, ArrayValue *, const Invocation *, const ArrayValue *);
const void *make_nested_list(Invocation *);
bool task_project_tail(Invocation *, const List *, List *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "task array ownership/call depth leaked");
}
static ArrayValue fixture(Invocation &source, unsigned depth = 3) {
  StringDescriptor name;
  kalandra_string_literal_v1(&source, &name, "é", 2);
  StringArrayDescriptor children{};
  for (unsigned n = 0; n < depth; ++n) {
    auto *cell = static_cast<Tree *>(source.allocateRecord(sizeof(Tree)));
    require(cell, "task tree fixture allocation failed");
    *cell = {name, children};
    children = {cell, cell, 0, 1, 1};
  }
  return {{}, children};
}
static void verify(const ArrayValue &value, unsigned depth) {
  auto array = value.items;
  for (unsigned n = 0; n < depth; ++n) {
    require(array.size == 1 && array.data, "task tree descriptor changed");
    const auto &cell = *static_cast<const Tree *>(array.data);
    require(cell.name.byteLength == 2 && !std::memcmp(cell.name.data, "é", 2),
            "task tree String lost");
    array = cell.children;
  }
  require(!array.data && !array.size, "task tree leaf changed");
}
static bool copy(Invocation &destination, void *output, const Invocation &source,
                 const void *input) noexcept {
  return task_copy_tree_array(&destination, static_cast<ArrayValue *>(output), &source,
                              static_cast<const ArrayValue *>(input));
}
static void nothing(void *) noexcept {}
static const ValueLayout layout{84001, sizeof(ArrayValue), alignof(ArrayValue), copy, nothing};
static bool construct(Invocation &destination, void *frame, const Packet &packet) {
  return packet.source && copy(destination, frame, *packet.source, packet.data);
}
static void resume(Task &task) {
  verify(*static_cast<const ArrayValue *>(task.frame()), 3);
  task.complete(task.frame());
}
static const TaskEntry entry{1,
                             84002,
                             sizeof(ArrayValue),
                             alignof(ArrayValue),
                             sizeof(ArrayValue),
                             alignof(ArrayValue),
                             construct,
                             nothing,
                             resume,
                             &layout};
int main() {
  try {
    for (unsigned mode = 0; mode < 13; ++mode) {
      Invocation source, destination, foreign;
      auto input = fixture(source);
      auto *root = static_cast<Tree *>(input.items.data);
      if (mode == 1 || mode == 12)
        input.items = {};
      if (mode == 2)
        input.items.stride = 2;
      if (mode == 3)
        root->children.allocated = root->children.data = reinterpret_cast<void *>(0x1230);
      if (mode == 4)
        root->children = fixture(foreign).items;
      if (mode == 5)
        root->name.byteLength = 100;
      std::atomic<bool> cancelled{mode == 6 || mode == 12};
      destination.bindCancellation(&cancelled);
      if (mode == 7)
        destination.fault(5);
      if (mode == 8)
        destination.expectedTaskError(1, 2, 0, 0);
      if (mode == 9)
        destination.expectedApplicationError("Problem", "Invalid");
      if (mode == 10)
        destination.settle();
      if (mode == 11)
        source.settle();
      ArrayValue output;
      std::memset(&output, 0x5a, sizeof(output));
      const auto before = output;
      const bool ok = task_copy_tree_array(&destination, &output, &source, &input);
      require(ok == (mode < 2), "invalid task tree accepted");
      if (ok) {
        source.settle();
        verify(output, mode == 1 ? 0 : 3);
      } else
        require(!std::memcmp(&before, &output, sizeof(output)), "failed array copy published");
      source.settle();
      destination.settle();
      foreign.settle();
      balanced(source);
      balanced(destination);
      balanced(foreign);
    }
    bool succeeded = false;
    for (unsigned attempt = 1; attempt < 20 && !succeeded; ++attempt) {
      Invocation source, destination;
      auto input = fixture(source);
      auto observer = source.lease(input.items.allocated);
      destination.failAllocationAt(attempt);
      ArrayValue output{};
      succeeded = task_copy_tree_array(&destination, &output, &source, &input);
      if (succeeded)
        verify(output, 3);
      else
        require(destination.status() == 2 && !output.items.data,
                "task array allocation failure published or changed error");
      destination.settle();
      require(observer->referenceCount() == 2, "partial task array retain leaked");
      source.settle();
      balanced(source);
      balanced(destination);
    }
    require(succeeded, "task tree allocation sweep never completed");
    for (unsigned depth : {128u, 512u}) {
      Invocation source, destination;
      auto input = fixture(source, depth);
      ArrayValue output{};
      const bool ok = task_copy_tree_array(&destination, &output, &source, &input);
      require(ok && !destination.status(), "valid deep task tree retained a call cap");
      if (ok) {
        source.settle();
        verify(output, depth);
      } else
        require(destination.status() == 6 && !output.items.data, "deep tree fault changed");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    // Repeated children are a shared DAG, not an active-path cycle.
    {
      Invocation source, destination;
      const auto leaf = fixture(source, 1);
      const auto name = static_cast<const Tree *>(leaf.items.data)->name;
      auto *cells = static_cast<Tree *>(source.allocateRecord(2 * sizeof(Tree)));
      require(cells, "shared task tree fixture allocation failed");
      cells[0] = cells[1] = {name, leaf.items};
      ArrayValue input{{}, {cells, cells, 0, 2, 1}}, output{};
      require(task_copy_tree_array(&destination, &output, &source, &input) && !destination.status(),
              "shared DAG child was treated as a cycle");
      source.settle();
      const auto *copied = static_cast<const Tree *>(output.items.data);
      require(output.items.size == 2,
              "shared task tree values changed");
      for (unsigned n = 0; n < 2; ++n) {
        ArrayValue branch{{}, {const_cast<Tree *>(copied + n), const_cast<Tree *>(copied + n), 0, 1, 1}};
        verify(branch, 2);
      }
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    // Reject a malformed active-path cycle independently of recursion depth.
    {
      Invocation source, destination;
      auto input = fixture(source, 1);
      static_cast<Tree *>(input.items.data)->children = input.items;
      ArrayValue output{};
      require(!task_copy_tree_array(&destination, &output, &source, &input) &&
                  destination.status() == 3 && !output.items.data,
              "cyclic task array was not rejected");
      source.settle();
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    {
      Invocation source, destination;
      auto *node = make_nested_list(&source);
      auto retained = source.lease(node);
      auto *cells = static_cast<List *>(source.allocateRecord(2 * sizeof(List)));
      require(retained && cells, "list array fixture failed");
      std::memcpy(cells, retained->bytes().data(), sizeof(List));
      cells[1] = {1, 0, reinterpret_cast<void *>(0x1230)}; // Inactive payload is ignored.
      ArrayValue input{{}, {cells, cells, 0, 2, 1}}, output{};
      require(task_copy_list_array(&destination, &output, &source, &input),
              "managed child in task array failed");
      source.settle();
      List child{};
      const auto *values = static_cast<const List *>(output.items.data);
      require(task_project_tail(&destination, &values[0], &child) && child.head == 42,
              "task array managed child lost after source settlement");
      destination.settle();
      balanced(source);
      balanced(destination);
    }
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "task array core creation failed");
      Invocation source, observed, survivor;
      auto input = fixture(source);
      auto group = core->openRoot();
      auto task = core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
      source.settle();
      require(task && core->drain() && core->failure(task).kind == TaskFailure::None,
              "task array execution failed");
      ArrayValue first{}, second{};
      require(core->observe(task, layout, observed, &first), "task array observation failed");
      const auto owners = observed.live();
      require(core->observe(task, layout, observed, &second) && observed.live() == 2 * owners &&
                  core->observe(task, layout, survivor, &second),
              "repeated/independent array observation failed");
      require(core->close(group) && core->drain() && core->release(group), "task array cleanup");
      const auto stats = core->stats();
      require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups,
              "task array scheduler leaked");
      core.reset();
      verify(first, 3);
      verify(second, 3);
      observed.settle();
      survivor.settle();
      balanced(source);
      balanced(observed);
      balanced(survivor);
    }
    std::cout << "recursive task arrays passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
