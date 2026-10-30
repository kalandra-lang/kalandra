#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" {
bool copy_list(const Invocation *, const void *, void *, Invocation *);
bool copy_callable(const Invocation *, const void *, void *, Invocation *);
bool copy_function_array(const Invocation *, const void *, void *, Invocation *);
bool copy_tree(const Invocation *, const void *, void *, Invocation *);
const void *make_list(Invocation *);
const void *callable_box(const void *, const void *, std::int64_t, Invocation *);
const void *function_array_box(const void *, std::int64_t, Invocation *);
const void *tree_array_box(const void *, Invocation *);
std::int64_t read_list_head(const void *, Invocation *);
const void *read_callable_environment(const void *, Invocation *);
const void *read_function_array_data(const void *, Invocation *);
const void *read_tree_data(const void *, Invocation *);
}
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(const Invocation &context) {
  require(!context.live() && !context.callDepth() && context.allocations() == context.releases(),
          "packet invocation leaked");
}
static void clean(const TaskCore &core) {
  const auto stats = core.stats();
  require(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished && !stats.records &&
              !stats.groups && !stats.environmentBytes && !stats.recursiveBytes,
          "recursive packet scheduler leaked");
}
struct Array {
  void *allocated, *data;
  std::int64_t offset, size, stride;
};
struct Cell {
  const void *code, *environment;
};
struct Tree {
  StringDescriptor name;
  Array children;
};
static const char environmentKey = 0, codeKey = 0;
static const void *fixture(Invocation &source, unsigned kind) {
  if (!kind)
    return make_list(&source);
  StringDescriptor text;
  kalandra_string_literal_v1(&source, &text, "é", 2);
  if (kind == 3) {
    const Tree tree{text, {}};
    auto *data = source.allocateRecord(sizeof(tree));
    require(data != nullptr, "tree array fixture allocation failed");
    std::memcpy(data, &tree, sizeof(tree));
    Array array{data, data, 0, 1, 1};
    return tree_array_box(&array, &source);
  }
  const void *dependency = text.allocated;
  auto *env = source.makeRecursiveValueFromPointers(std::as_bytes(std::span{&text, 1}),
                                                    &environmentKey, {&dependency, 1});
  require(env && source.bindRecursiveEnvironment(env, &codeKey), "closure fixture failed");
  if (kind == 1)
    return callable_box(&codeKey, env, 2, &source);
  const Cell cells[] = {{&codeKey, env}, {&codeKey, env}, {&codeKey, nullptr}};
  auto *data = source.allocateRecord(sizeof(cells));
  require(data != nullptr, "function array fixture allocation failed");
  std::memcpy(data, cells, sizeof(cells));
  Array array{data, data, 0, 3, 1};
  return function_array_box(&array, 2, &source);
}
using Copier = bool (*)(const Invocation *, const void *, void *, Invocation *);
static const std::array<Copier, 4> copiers{copy_list, copy_callable, copy_function_array,
                                           copy_tree};
static void verify(Invocation &context, const void *root, unsigned kind) {
  if (!kind) {
    require(read_list_head(root, &context) == 42 && !context.status(), "worker list read failed");
    return;
  }
  StringDescriptor text;
  if (kind == 3) {
    auto *data = read_tree_data(root, &context);
    require(data && context.lease(data), "worker tree array backing missing");
    Tree tree;
    std::memcpy(&tree, data, sizeof(tree));
    text = tree.name;
  } else {
    const void *env = nullptr;
    if (kind == 1)
      env = read_callable_environment(root, &context);
    else {
      auto *data = read_function_array_data(root, &context);
      require(data && context.lease(data), "worker function array backing missing");
      Cell cell;
      std::memcpy(&cell, data, sizeof(cell));
      env = cell.environment;
    }
    auto *data = context.readRecursiveEnvironment(env, sizeof(text), &codeKey);
    require(data != nullptr, "worker closure environment missing");
    std::memcpy(&text, data, sizeof(text));
  }
  require(kalandra_string_length_v1(&context, &text) == 1 && !context.status(),
          "worker nested String backing missing");
}
static std::atomic<unsigned> copies{0}, failCopy{0};
template <unsigned Kind>
static bool copy(Invocation &destination, void *output, const Invocation &source,
                 const void *input) noexcept {
  const auto attempt = copies.fetch_add(1, std::memory_order_relaxed) + 1;
  if (attempt == failCopy.load(std::memory_order_relaxed))
    destination.failAllocationAt(1);
  const void *root;
  std::memcpy(&root, input, sizeof(root));
  return copiers[Kind](&source, root, output, &destination);
}
static void nothing(void *) noexcept {}
static const std::array<ValueLayout, 4> layouts{
    {{81001, sizeof(void *), alignof(void *), copy<0>, nothing},
     {81002, sizeof(void *), alignof(void *), copy<1>, nothing},
     {81003, sizeof(void *), alignof(void *), copy<2>, nothing},
     {81004, sizeof(void *), alignof(void *), copy<3>, nothing}}};
struct Input {
  const void *root;
  unsigned kind, mode;
};
static bool construct(Invocation &context, void *output, const Packet &packet) {
  const auto &input = *static_cast<const Input *>(packet.data);
  Input frame{nullptr, input.kind, input.mode};
  if (!packet.source ||
      !layouts[input.kind].copy(context, &frame.root, *packet.source, &input.root))
    return false;
  new (output) Input(frame);
  return true;
}
static void resume(Task &task) {
  auto &input = *static_cast<Input *>(task.frame());
  verify(task.context(), input.root, input.kind);
  if (input.mode == 9)
    task.fault(5);
  else
    task.complete(&input.root);
}
int main() {
  try {
    for (unsigned kind = 0; kind < 4; ++kind) {
      // Generated callback failures never overwrite the caller's output slot.
      for (unsigned mode = 0; mode < 17; ++mode) {
        Invocation source, destination;
        const void *root = fixture(source, kind);
        require(root && !source.status(), "compiled fixture failed");
        const void *output = reinterpret_cast<void *>(0x5a5a);
        const auto sentinel = output;
        const Invocation *selected = &source;
        auto copier = copiers[kind];
        if (mode < 7)
          destination.failAllocationAt(mode + 1);
        if (mode == 7)
          copier = copiers[(kind + 1) % 4];
        if (mode == 8)
          root = reinterpret_cast<void *>(0x1230);
        if (mode == 9)
          selected = nullptr;
        if (mode == 10)
          source.settle();
        if (mode == 11)
          source.fault(5);
        std::atomic<bool> cancelled{mode == 12};
        destination.bindCancellation(&cancelled);
        if (mode == 13)
          destination.fault(5);
        if (mode == 14)
          destination.expectedTaskError(1, 2, 0, 0);
        if (mode == 15)
          destination.expectedApplicationError("Problem", "Invalid");
        if (mode == 16)
          destination.settle();
        const bool success = copier(selected, root, &output, &destination);
        if (success) {
          require(mode < 7 && output == root, "invalid packet callback succeeded");
          source.settle();
          verify(destination, output, kind);
        } else {
          require(output == sentinel && !destination.live(),
                  "failed packet callback published output");
          if (mode < 7)
            require(destination.status() == 2, "packet callback allocation failure lost");
          else if (mode < 12 || mode == 16)
            require(destination.status() == 3, "invalid packet callback fault changed");
          else if (mode == 13)
            require(destination.status() == 5, "destination fault lost");
          else if (mode == 14 || mode == 15)
            require(destination.hasExpectedError() && !destination.status(),
                    "destination expected error lost");
        }
        source.settle();
        destination.settle();
        balanced(source);
        balanced(destination);
      }
      // The compiler-generated graph copier must honor the same shared byte cap
      // at packet admission and at cached-result publication.
      for (unsigned workers : {1u, 2u, 4u}) {
        for (unsigned boundary = 0; boundary < 4; ++boundary) {
          copies.store(0);
          failCopy.store(0);
          Invocation source, probe, observer;
          Input input{fixture(source, kind), kind, 0};
          const void *out = nullptr;
          require(copiers[kind](&source, input.root, &out, &probe), "graph budget probe failed");
          const auto graphBytes = probe.recursiveBytes();
          require(graphBytes != 0, "generated copy did not reserve graph backing");
          probe.settle();
          const TaskEntry entry{
              1,         83001 + kind, sizeof(Input), alignof(Input), sizeof(Input), alignof(Input),
              construct, nothing,      resume,        &layouts[kind]};
          const auto fixed = entry.frameBytes + entry.result->bytes;
          TaskLimits limits;
          limits.workers = workers;
          limits.bytes =
              fixed + (boundary < 2 ? graphBytes : 2 * graphBytes) - (boundary % 2 ? 0 : 1);
          auto core = TaskCore::create(limits);
          require(bool(core), "bounded graph task core failed");
          auto group = core->openRoot();
          auto task =
              core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
          source.settle();
          require(core->drain(), "bounded graph task drain failed");
          auto failure = core->failure(group);
          if (failure.kind == TaskFailure::None && task)
            failure = core->failure(task);
          out = reinterpret_cast<void *>(0x5a5a);
          if (boundary == 3) {
            require(task && failure.kind == TaskFailure::None &&
                        core->observe(task, layouts[kind], observer, &out),
                    "exact packet/result graph byte boundary rejected");
            verify(observer, out, kind);
            const auto stats = core->stats();
            require(stats.peakBytes == limits.bytes && stats.recursiveBytes == graphBytes &&
                        stats.recursiveBytes <= stats.retainedBytes,
                    "graph reservations not reflected in scheduler statistics");
          } else {
            require(failure.kind == TaskFailure::Fault && failure.code == 2 &&
                        (!task || !core->observe(task, layouts[kind], observer, &out)) &&
                        out == reinterpret_cast<void *>(0x5a5a),
                    "over-budget graph published a task result");
            require(copies.load() == (boundary == 0 ? 1u : 2u),
                    "graph budget failure happened at wrong boundary");
          }
          require(core->close(group) && core->drain() && core->release(group),
                  "bounded graph group cleanup failed");
          clean(*core);
          core.reset();
          if (boundary == 3)
            verify(observer, out, kind);
          observer.settle();
          balanced(source);
          balanced(observer);
        }
      }
      for (unsigned workers : {1u, 2u, 4u}) {
        for (unsigned mode = 0; mode < 10; ++mode) {
          copies.store(0);
          failCopy.store(mode == 5 ? 1 : mode == 6 ? 2 : 0);
          TaskLimits limits;
          limits.workers = workers;
          auto core = TaskCore::create(limits);
          require(bool(core), "task core creation failed");
          Invocation source, destination, survivor;
          Input input{fixture(source, kind), kind, mode};
          require(input.root && !source.status(), "scheduler fixture failed");
          auto *initial = input.root;
          if (mode == 3)
            source.fault(5);
          if (mode == 4)
            input.root = reinterpret_cast<void *>(0x1230);
          const TaskEntry entry{
              1,         82001 + kind, sizeof(Input), alignof(Input), sizeof(Input), alignof(Input),
              construct, nothing,      resume,        &layouts[kind]};
          auto group = core->openRoot();
          if (mode == 8)
            core->failAllocationAt(1);
          auto task =
              core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
          input = {};
          source.settle();
          if (mode == 1 || mode == 2)
            require(core->cancel(group, mode == 2), "packet task cancellation failed");
          require(core->drain(), "packet task drain failed");
          auto failure = core->failure(group);
          if (failure.kind == TaskFailure::None && task)
            failure = core->failure(task);
          const bool successful = mode == 0 || mode == 7;
          if (successful) {
            require(task && failure.kind == TaskFailure::None, "recursive packet task failed");
            if (mode == 7) {
              destination.failAllocationAt(1);
              const void *out = reinterpret_cast<void *>(0x5a5a);
              require(!core->observe(task, layouts[kind], destination, &out) &&
                          destination.status() == 2 && out == reinterpret_cast<void *>(0x5a5a),
                      "failed cached observation published output");
              destination.settle();
            } else {
              const void *out = nullptr;
              require(core->observe(task, layouts[kind], destination, &out),
                      "first observation failed");
              verify(destination, out, kind);
              const auto count = destination.live();
              require(core->observe(task, layouts[kind], destination, &out) &&
                          destination.live() == count,
                      "repeated observation duplicated owners");
              verify(destination, out, kind);
            }
            const void *out = nullptr;
            require(core->observe(task, layouts[kind], survivor, &out) && out == initial,
                    "independent observer failed");
            require(core->close(group) && core->drain() && core->release(group),
                    "successful group cleanup");
            clean(*core);
            core.reset();
            verify(survivor, out, kind);
            // A later task may consume an observed result after the original core is gone.
            auto onward = TaskCore::create(limits);
            auto nextGroup = onward->openRoot();
            Input next{out, kind, 0};
            auto nextTask = onward->submit(nextGroup, entry,
                                           {entry.packetLayout, &next, sizeof(next), &survivor});
            survivor.settle();
            require(nextTask && onward->drain() &&
                        onward->failure(nextTask).kind == TaskFailure::None,
                    "onward packet submission failed");
            Invocation final;
            require(onward->observe(nextTask, layouts[kind], final, &out),
                    "onward observation failed");
            require(onward->close(nextGroup) && onward->drain() && onward->release(nextGroup),
                    "onward group cleanup failed");
            clean(*onward);
            onward.reset();
            verify(final, out, kind);
            final.settle();
            balanced(final);
          } else {
            require(failure.kind != TaskFailure::None, "failed packet task published success");
            if (mode == 1 || mode == 2)
              require(failure.kind ==
                          (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
                      "packet cancellation variant changed");
            else if (mode == 3 || mode == 4)
              require(failure.kind == TaskFailure::Fault && failure.code == 3,
                      "invalid packet admitted");
            else if (mode == 5 || mode == 6)
              require(failure.kind == TaskFailure::Fault && failure.code == 2,
                      "packet allocation failure lost");
            else if (mode == 9)
              require(failure.kind == TaskFailure::Fault && failure.code == 5,
                      "worker body fault changed");
            const void *out = reinterpret_cast<void *>(0x5a5a);
            require(!task || !core->observe(task, layouts[kind], survivor, &out),
                    "failed task exposed a recursive result");
            require(out == reinterpret_cast<void *>(0x5a5a), "failed observation changed output");
            require(core->close(group) && core->drain() && core->release(group),
                    "failed group cleanup");
            auto stats = core->stats();
            require(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished &&
                        !stats.records && !stats.groups && !stats.environmentBytes &&
                        !stats.recursiveBytes,
                    "recursive packet scheduler leaked");
          }
          destination.settle();
          survivor.settle();
          balanced(source);
          balanced(destination);
          balanced(survivor);
        }
      }
    }
    std::cout << "recursive packet bridge passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
