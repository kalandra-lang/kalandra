#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace kalandra::runtime;
static void require(bool condition, int line) {
  if (!condition) {
    std::fprintf(stderr, "Task assertion at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(x) require(bool(x), __LINE__)
static void nothing(void *) noexcept {}
static bool copyFloat(Invocation &, void *output, const Invocation &, const void *input) noexcept {
  *static_cast<float *>(output) = *static_cast<const float *>(input);
  return true;
}
static const ValueLayout scalar{1, sizeof(float), alignof(float), copyFloat, nothing};
struct Input {
  float value;
  unsigned yields;
  int fault;
  unsigned *runs;
};
struct Leaf {
  Input input;
};
static bool constructLeaf(Invocation &, void *frame, const Packet &packet) {
  new (frame) Leaf{*static_cast<const Input *>(packet.data)};
  return true;
}
static void resumeLeaf(Task &task) {
  auto &input = static_cast<Leaf *>(task.frame())->input;
  ++*input.runs;
  if (input.fault) {
    task.fault(input.fault, 17, 4);
    return;
  }
  if (input.yields) {
    --input.yields;
    task.yield();
    return;
  }
  task.complete(&input.value);
}
static const TaskEntry leaf{
    1,       10,         sizeof(Input), alignof(Input), sizeof(Leaf), alignof(Leaf), constructLeaf,
    nothing, resumeLeaf, &scalar};
static Packet packet(const Input &input) { return {10, &input, sizeof(input)}; }
struct BranchInput {
  unsigned depth;
  bool recover;
  unsigned *runs;
};
struct Branch {
  BranchInput input;
  unsigned phase = 0;
  GroupHandle group{};
  TaskHandle first{}, second{};
  float a = 0, b = 0;
};
static const TaskEntry &branchEntry();
static bool constructBranch(Invocation &, void *frame, const Packet &packet) {
  new (frame) Branch{*static_cast<const BranchInput *>(packet.data)};
  return true;
}
static void resumeBranch(Task &task) {
  auto &frame = *static_cast<Branch *>(task.frame());
  if (frame.phase == 0) {
    frame.group = task.openGroup();
    if (!frame.group)
      return;
    if (frame.input.depth) {
      BranchInput input{frame.input.depth - 1, frame.input.recover, frame.input.runs};
      frame.first = task.spawn(frame.group, branchEntry(), {20, &input, sizeof(input)});
    } else {
      Input input{2.0f, 2, frame.input.recover ? 1 : 0, frame.input.runs};
      frame.first = task.spawn(frame.group, leaf, packet(input));
    }
    Input input{3.0f, 1, 0, frame.input.runs};
    frame.second = task.spawn(frame.group, leaf, packet(input));
    frame.phase = 1;
  }
  if (frame.phase == 1) {
    if (!task.await(frame.first))
      return;
    // For failure recovery, join the now-failing inner group rather than reading success.
    if (task.failure(frame.group).kind == TaskFailure::None) {
      CHECK(task.read(frame.first, scalar, &frame.a));
      float again = 0;
      CHECK(task.await(frame.first));
      CHECK(task.read(frame.first, scalar, &again) && again == frame.a);
    }
    frame.phase = 2;
  }
  if (frame.phase == 2) {
    if (!task.await(frame.second))
      return;
    if (task.failure(frame.group).kind == TaskFailure::None)
      CHECK(task.read(frame.second, scalar, &frame.b));
    CHECK(task.close(frame.group));
    frame.phase = 3;
  }
  if (!task.join(frame.group))
    return;
  const float result =
      task.failure(frame.group).kind == TaskFailure::None ? frame.a + frame.b : 99.0f;
  task.complete(&result);
}
static const TaskEntry &branchEntry() {
  static const TaskEntry entry{1,
                               20,
                               sizeof(BranchInput),
                               alignof(BranchInput),
                               sizeof(Branch),
                               alignof(Branch),
                               constructBranch,
                               nothing,
                               resumeBranch,
                               &scalar};
  return entry;
}
struct Tensor {
  void *backing;
  float *values;
};
static bool copyTensor(Invocation &destination, void *output, const Invocation &source,
                       const void *input) noexcept {
  auto tensor = *static_cast<const Tensor *>(input);
  if (!destination.retainFrom(source, tensor.backing))
    return false;
  *static_cast<Tensor *>(output) = tensor;
  return true;
}
static const ValueLayout tensorLayout{2, sizeof(Tensor), alignof(Tensor), copyTensor, nothing};
struct TensorInput {
  Invocation *owner;
  Tensor value;
  unsigned *destroyed;
};
struct TensorFrame {
  TensorInput input;
};
static bool constructTensor(Invocation &context, void *frame, const Packet &packet) {
  auto input = *static_cast<const TensorInput *>(packet.data);
  if (!context.retainFrom(*input.owner, input.value.backing))
    return false;
  new (frame) TensorFrame{input};
  return true;
}
static void destroyTensor(void *frame) noexcept {
  ++*static_cast<TensorFrame *>(frame)->input.destroyed;
}
static void resumeTensor(Task &task) {
  const auto &input = static_cast<TensorFrame *>(task.frame())->input;
  task.complete(&input.value);
}
static const TaskEntry tensorEntry{1,
                                   30,
                                   sizeof(TensorInput),
                                   alignof(TensorInput),
                                   sizeof(TensorFrame),
                                   alignof(TensorFrame),
                                   constructTensor,
                                   destroyTensor,
                                   resumeTensor,
                                   &tensorLayout};
// One-child continuation chain also tests ready-queue capacity one.
struct ChainInput {
  unsigned depth;
  bool yieldBeforeAwait;
};
struct ChainFrame {
  ChainInput input;
  GroupHandle group{};
  TaskHandle child{};
  unsigned phase = 0;
};
static const TaskEntry &chainEntry();
static bool constructChain(Invocation &, void *frame, const Packet &packet) {
  new (frame) ChainFrame{*static_cast<const ChainInput *>(packet.data)};
  return true;
}
static void resumeChain(Task &task) {
  auto &frame = *static_cast<ChainFrame *>(task.frame());
  if (!frame.input.depth) {
    float value = 1;
    task.complete(&value);
    return;
  }
  if (!frame.phase) {
    frame.group = task.openGroup();
    if (!frame.group)
      return;
    ChainInput input{frame.input.depth - 1, frame.input.yieldBeforeAwait};
    frame.child = task.spawn(frame.group, chainEntry(), {40, &input, sizeof(input)});
    frame.phase = 1;
    if (frame.input.yieldBeforeAwait) {
      task.yield();
      return;
    }
  }
  if (frame.phase == 1) {
    if (!task.await(frame.child))
      return;
    if (task.failure(frame.child).kind != TaskFailure::None) {
      task.fault(3);
      return;
    }
    CHECK(task.close(frame.group));
    frame.phase = 2;
  }
  if (!task.join(frame.group))
    return;
  float value = 0;
  CHECK(task.read(frame.child, scalar, &value));
  value += 1;
  task.complete(&value);
}
static const TaskEntry &chainEntry() {
  static const TaskEntry entry{1,
                               40,
                               sizeof(ChainInput),
                               alignof(ChainInput),
                               sizeof(ChainFrame),
                               alignof(ChainFrame),
                               constructChain,
                               nothing,
                               resumeChain,
                               &scalar};
  return entry;
}
static bool partialConstruct(Invocation &context, void *, const Packet &packet) {
  auto input = *static_cast<const TensorInput *>(packet.data);
  CHECK(context.retainFrom(*input.owner, input.value.backing));
  context.fault(2);
  return false;
}
static bool partialCopy(Invocation &destination, void *, const Invocation &source,
                        const void *input) noexcept {
  auto value = *static_cast<const Tensor *>(input);
  if (!destination.retainFrom(source, value.backing))
    return false;
  destination.fault(2);
  return false;
}
static const ValueLayout failingLayout{3, sizeof(Tensor), alignof(Tensor), partialCopy, nothing};
static const TaskEntry partialPacket{1,
                                     30,
                                     sizeof(TensorInput),
                                     alignof(TensorInput),
                                     sizeof(TensorFrame),
                                     alignof(TensorFrame),
                                     partialConstruct,
                                     destroyTensor,
                                     resumeTensor,
                                     &tensorLayout};
static const TaskEntry partialOutcome{1,
                                      30,
                                      sizeof(TensorInput),
                                      alignof(TensorInput),
                                      sizeof(TensorFrame),
                                      alignof(TensorFrame),
                                      constructTensor,
                                      destroyTensor,
                                      resumeTensor,
                                      &failingLayout};
static void throws(Task &) { throw 23; }
static const TaskEntry throwingEntry{
    1,       10,     sizeof(Input), alignof(Input), sizeof(Leaf), alignof(Leaf), constructLeaf,
    nothing, throws, &scalar};
static void bodyFault(Task &task) {
  auto input = static_cast<Leaf *>(task.frame())->input;
  auto group = task.openGroup();
  CHECK(group);
  CHECK(task.spawn(group, leaf, packet(input)));
  task.fault(1, 11, 8);
}
static const TaskEntry bodyFaultEntry{
    1,       10,        sizeof(Input), alignof(Input), sizeof(Leaf), alignof(Leaf), constructLeaf,
    nothing, bodyFault, &scalar};
struct LexicalFrame {
  Input input;
  GroupHandle outer{}, inner{};
  unsigned phase = 0;
};
static bool constructLexical(Invocation &, void *frame, const Packet &packet) {
  new (frame) LexicalFrame{*static_cast<const Input *>(packet.data)};
  return true;
}
static void resumeLexical(Task &task) {
  auto &frame = *static_cast<LexicalFrame *>(task.frame());
  if (!frame.phase) {
    frame.outer = task.openGroup();
    frame.inner = task.openGroup(frame.outer);
    CHECK(frame.outer && frame.inner);
    CHECK(task.spawn(frame.inner, leaf, packet(frame.input)));
    CHECK(task.cancel(frame.outer));
    frame.phase = 1;
  }
  if (frame.phase == 1) {
    CHECK(task.close(frame.inner));
    if (!task.join(frame.inner))
      return;
    CHECK(task.failure(frame.inner).kind == TaskFailure::Cancelled);
    frame.phase = 2;
  }
  CHECK(task.close(frame.outer));
  if (!task.join(frame.outer))
    return;
  float result = 5;
  task.complete(&result);
}
static const TaskEntry lexicalEntry{1,
                                    10,
                                    sizeof(Input),
                                    alignof(Input),
                                    sizeof(LexicalFrame),
                                    alignof(LexicalFrame),
                                    constructLexical,
                                    nothing,
                                    resumeLexical,
                                    &scalar};
// The compiler keeps an application payload in its own typed frame. The
// scheduler only reserves the primary outcome and drains descendants.
static void resumeStop(Task &task) {
  auto &frame = *static_cast<LexicalFrame *>(task.frame());
  if (!frame.phase) {
    frame.outer = task.openGroup();
    auto child = frame.input;
    child.fault = 0;
    CHECK(task.spawn(frame.outer, leaf, packet(child)));
    if (frame.input.fault) {
      CHECK(task.failGroup(frame.outer, {TaskFailure(frame.input.fault), 42, 17, 4}));
      CHECK(!task.stopGroup(frame.outer));
      const auto failure = task.failure(frame.outer);
      CHECK(failure.kind == TaskFailure(frame.input.fault) && failure.code == 42 &&
            failure.line == 17 && failure.column == 4);
    } else {
      CHECK(task.stopGroup(frame.outer));
      CHECK(!task.stopGroup(frame.outer));
      CHECK(task.failure(frame.outer).kind == TaskFailure::Cancelled);
    }
    CHECK(!task.stopGroup({}));
    CHECK(task.close(frame.outer));
    frame.phase = 1;
  }
  if (!task.join(frame.outer))
    return;
  CHECK(!task.stopGroup(frame.outer));
  float value = 42;
  task.complete(&value);
}
static const TaskEntry stopEntry{1,
                                 10,
                                 sizeof(Input),
                                 alignof(Input),
                                 sizeof(LexicalFrame),
                                 alignof(LexicalFrame),
                                 constructLexical,
                                 nothing,
                                 resumeStop,
                                 &scalar};
static void empty(TaskCore &core) {
  auto stats = core.stats();
  CHECK(stats.unfinished == 0 && stats.records == 0 && stats.groups == 0 &&
        stats.retainedBytes == 0 && stats.retainedOwners == 0 && !stats.ready && !stats.pending &&
        !stats.maintenanceTasks);
}
struct PropagationInput {
  Failure failure;
  bool ownFault;
};
static bool constructPropagation(Invocation &, void *frame, const Packet &packet) {
  new (frame) PropagationInput{*static_cast<const PropagationInput *>(packet.data)};
  return true;
}
static void resumePropagation(Task &task) {
  auto &input = *static_cast<PropagationInput *>(task.frame());
  if (input.ownFault)
    task.context().fault(2, 7, 8);
  task.fail(input.failure);
}
static const TaskEntry propagation{1,
                                   99,
                                   sizeof(PropagationInput),
                                   alignof(PropagationInput),
                                   sizeof(PropagationInput),
                                   alignof(PropagationInput),
                                   constructPropagation,
                                   nothing,
                                   resumePropagation,
                                   &scalar};
static void resumeAllocatedBuffers(Task &task) {
  StringDescriptor text{};
  kalandra_string_literal_v1(&task.context(), &text, "é", 2);
  if (task.context().poll())
    return;
  auto *values = static_cast<float *>(task.context().allocate(2 * sizeof(float)));
  if (!values)
    return;
  values[0] = static_cast<float>(kalandra_string_length_v1(&task.context(), &text));
  values[1] = 34;
  Tensor result{values, values};
  task.complete(&result);
}
static void allocatedBufferBudgets() {
  const TaskEntry entry{1,
                        10,
                        sizeof(Input),
                        alignof(Input),
                        sizeof(Leaf),
                        alignof(Leaf),
                        constructLeaf,
                        nothing,
                        resumeAllocatedBuffers,
                        &tensorLayout};
  for (unsigned workers : {1u, 2u, 4u}) {
    std::size_t admitted = 0, peak = 0;
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
      TaskLimits limits;
      limits.workers = workers;
      if (boundary)
        limits.bytes = boundary == 1 ? admitted : peak - (boundary == 2 ? 1 : 0);
      auto core = TaskCore::create(limits);
      CHECK(core);
      unsigned runs = 0;
      Input input{0, 0, 0, &runs};
      auto group = core->openRoot();
      auto task = core->submit(group, entry, packet(input));
      CHECK(task);
      if (!boundary)
        admitted = core->stats().retainedBytes;
      else
        CHECK(core->stats().retainedBytes == admitted);
      CHECK(core->drain());
      if (!boundary)
        peak = core->stats().peakBytes;
      CHECK(peak > admitted && core->stats().peakBytes <= limits.bytes);
      Invocation receiver;
      Tensor result{nullptr, reinterpret_cast<float *>(0x1230)}, cached{};
      if (boundary == 1 || boundary == 2) {
        const auto failure = core->failure(task);
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        CHECK(!core->observe(task, tensorLayout, receiver, &result) &&
              result.values == reinterpret_cast<float *>(0x1230));
      } else {
        CHECK(core->failure(task).kind == TaskFailure::None);
        CHECK(core->observe(task, tensorLayout, receiver, &result) &&
              core->observe(task, tensorLayout, receiver, &cached));
      }
      CHECK(core->close(group) && core->drain() && core->release(group));
      empty(*core);
      CHECK(!core->stats().recursiveBytes && !core->stats().environmentBytes);
      core.reset();
      if (!boundary || boundary == 3)
        CHECK(result.values[0] == 1 && cached.values[1] == 34);
      receiver.settle();
      CHECK(!receiver.live() && receiver.allocations() == receiver.releases());
    }
  }
}
int main() {
  allocatedBufferBudgets();
  // Propagated errors preserve their variant and location. Recovery must never
  // erase a fault already raised in the receiving invocation.
  for (unsigned kind : {0u, 1u, 2u, 3u, 4u, 99u}) {
    for (bool ownFault : {false, true}) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      PropagationInput input{{TaskFailure(kind), 17, 19, 23}, ownFault};
      auto task = core->submit(group, propagation, {99, &input, sizeof(input)});
      CHECK(task && core->close(group) && core->drain());
      auto failure = core->failure(task);
      if (ownFault) {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2 && failure.line == 7 &&
              failure.column == 8);
      } else if (kind < 1 || kind > 4) {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 3);
      } else {
        CHECK(failure.kind == TaskFailure(kind) && failure.code == 17 && failure.line == 19 &&
              failure.column == 23);
      }
      CHECK(core->release(group));
      CHECK(!core->stats().records && !core->stats().groups && !core->stats().retainedBytes);
    }
  }
  CHECK(!TaskCore::create({0, 1, 1, 1, 1, 1}));
  auto core = TaskCore::create();
  CHECK(core);
  unsigned runs = 0;
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    auto root = core->openRoot();
    BranchInput input{8, false, &runs};
    auto task = core->submit(root, branchEntry(), {20, &input, sizeof(input)});
    CHECK(core->drain());
    CHECK(core->settled(task) && core->failure(task).kind == TaskFailure::None);
    Invocation receiver;
    float value = 0;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 29.0f);
    const auto resumes = core->stats().resumes;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 29.0f);
    CHECK(core->stats().resumes == resumes);
    CHECK(core->close(root) && core->drain() && core->settled(root));
    CHECK(core->failure(root).kind == TaskFailure::None);
    CHECK(core->release(root));
    // Generation-checked stale handles cannot access a reused slot.
    Invocation stale;
    CHECK(!core->observe(task, scalar, stale, &value) && stale.status() == 3);
    empty(*core);
  }
  for (unsigned capacity : {16U, 32U, 33U, 128U}) {
    for (unsigned depth : {12U, 20U, 70U}) {
      if (depth >= capacity)
        continue;
      for (bool yield : {false, true}) {
        TaskLimits limits;
        limits.records = capacity;
        limits.ready = 1;
        limits.groups = 128;
        limits.inFlight = 128;
        limits.depth = 100;
        auto narrow = TaskCore::create(limits);
        auto root = narrow->openRoot();
        ChainInput input{depth, yield};
        auto task = narrow->submit(root, chainEntry(), {40, &input, sizeof(input)});
        CHECK(narrow->drain());
        float result = 0;
        Invocation receiver;
        CHECK(narrow->observe(task, scalar, receiver, &result) && result == depth + 1);
        CHECK(narrow->close(root) && narrow->drain() && narrow->release(root));
        empty(*narrow);
      }
    }
  }
  // Only waits and unpublished settlements keep task maintenance active.
  // Cancellation wakes a waiting parent without losing or duplicating the count.
  for (unsigned action = 0; action < 3; ++action) {
    TaskLimits limits;
    limits.records = 16;
    auto small = TaskCore::create(limits);
    auto root = small->openRoot();
    ChainInput input{1, false};
    auto task = small->submit(root, chainEntry(), {40, &input, sizeof(input)});
    CHECK(small->stats().maintenanceTasks == 0);
    CHECK(small->step() && small->stats().maintenanceTasks == 1);
    if (action) {
      CHECK(small->cancel(root, action == 2));
      CHECK(small->stats().maintenanceTasks == 0);
      CHECK(small->drain());
      CHECK(small->failure(root).kind ==
            (action == 2 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    } else {
      CHECK(small->step() && small->stats().maintenanceTasks == 0);
      CHECK(small->drain());
      Invocation receiver;
      float result = 0;
      CHECK(small->observe(task, scalar, receiver, &result) && result == 2);
      CHECK(small->close(root) && small->drain());
    }
    CHECK(small->release(root));
    empty(*small);
  }
  // Sparse high slots cross bitmap words, including the emergency rejection
  // records. Reuse cleared words and reject stale handles after every cycle.
  {
    TaskLimits limits;
    limits.records = limits.groups = limits.ready = limits.inFlight = 130;
    auto sparse = TaskCore::create(limits);
    TaskHandle staleTask;
    GroupHandle staleGroup;
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
      GroupHandle roots[70];
      TaskHandle tasks[70];
      Input input{7, 0, 0, &runs};
      for (unsigned i = 0; i < 70; ++i) {
        roots[i] = sparse->openRoot();
        tasks[i] = sparse->submit(roots[i], leaf, packet(input));
        CHECK(roots[i] && tasks[i]);
      }
      CHECK(!staleTask || sparse->failure(staleTask).kind == TaskFailure::Fault);
      CHECK(!staleGroup || sparse->failure(staleGroup).kind == TaskFailure::Fault);
      for (unsigned i = 0; i < 65; ++i)
        CHECK(sparse->step());
      Invocation receiver;
      float result = 0;
      for (unsigned i = 0; i < 65; ++i) {
        CHECK(sparse->observe(tasks[i], scalar, receiver, &result) && result == 7);
        CHECK(sparse->close(roots[i]));
      }
      CHECK(!sparse->drain(0));
      for (unsigned i = 0; i < 65; ++i)
        CHECK(sparse->release(roots[i]));
      CHECK(sparse->stats().records == 5 && sparse->stats().groups == 5);
      CHECK(sparse->cancel(roots[66]) && sparse->cancel(roots[68], true));
      auto malformed = packet(input);
      malformed.layout = 999;
      const auto rejected = sparse->submit(roots[69], leaf, malformed);
      CHECK(rejected.slot >= limits.records && sparse->settled(rejected));
      CHECK(sparse->drain());
      for (unsigned i : {65U, 67U}) {
        CHECK(sparse->observe(tasks[i], scalar, receiver, &result) && result == 7);
        CHECK(sparse->close(roots[i]));
      }
      CHECK(sparse->failure(tasks[66]).kind == TaskFailure::Cancelled);
      CHECK(sparse->failure(roots[68]).kind == TaskFailure::DeadlineExceeded);
      CHECK(sparse->failure(rejected).kind == TaskFailure::AdmissionRejected);
      CHECK(sparse->drain());
      for (unsigned i = 65; i < 70; ++i)
        CHECK(sparse->release(roots[i]));
      staleTask = tasks[0];
      staleGroup = roots[0];
      empty(*sparse);
    }
  }
  // Descriptor/packet failures cannot invoke the task or retain payload storage.
  for (const auto *entry : {&partialPacket, &partialOutcome}) {
    Invocation parent;
    auto *data = static_cast<float *>(parent.allocate(4));
    CHECK(data);
    unsigned destroyed = 0;
    TensorInput input{&parent, {data, data}, &destroyed};
    auto root = core->openRoot();
    auto task = core->submit(root, *entry, {30, &input, sizeof(input)});
    CHECK(core->drain() && core->failure(task).kind == TaskFailure::Fault);
    CHECK(core->failure(task).code == 2 && core->stats().retainedOwners == 0);
    CHECK(destroyed == (entry == &partialOutcome ? 1U : 0U) && parent.live() == 1);
    CHECK(core->release(root));
    empty(*core);
  }
  // Unexpected host exceptions are contained; sibling cancellation preserves the primary fault.
  {
    auto root = core->openRoot();
    Input input{1, 0, 0, &runs};
    auto failed = core->submit(root, throwingEntry, packet(input));
    auto sibling = core->submit(root, leaf, packet(input));
    auto before = runs;
    CHECK(core->drain() && core->failure(failed).code == 3);
    CHECK(core->failure(sibling).kind == TaskFailure::Cancelled && runs == before);
    CHECK(core->failure(root).kind == TaskFailure::Fault && core->release(root));
  }
  // A completed task stays cached if its group is cancelled afterwards.
  {
    auto root = core->openRoot();
    Input input{4, 0, 0, &runs};
    auto task = core->submit(root, leaf, packet(input));
    CHECK(core->drain() && core->settled(task));
    CHECK(core->cancel(root) && core->drain());
    CHECK(core->failure(task).kind == TaskFailure::None);
    Invocation receiver;
    float value = 0;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 4);
    CHECK(core->failure(root).kind == TaskFailure::Cancelled && core->release(root));
  }
  // Nesting quota and missing group-control space fail without leaving a waiting parent.
  for (bool depth : {false, true}) {
    TaskLimits limits;
    if (depth)
      limits.depth = 0;
    else
      limits.groups = 1;
    auto small = TaskCore::create(limits);
    auto root = small->openRoot();
    BranchInput input{1, false, &runs};
    small->submit(root, branchEntry(), {20, &input, sizeof(input)});
    CHECK(small->drain());
    CHECK(small->failure(root).kind ==
          (depth ? TaskFailure::AdmissionRejected : TaskFailure::Fault));
    CHECK(small->release(root));
    empty(*small);
  }
  // Independent roots share the worker, not each other's failure/cancellation owner.
  {
    auto bad = core->openRoot(), good = core->openRoot();
    Input input{8, 0, 0, &runs};
    auto failed = core->submit(bad, bodyFaultEntry, packet(input));
    auto success = core->submit(good, leaf, packet(input));
    input.value = 900; // The submitted packet was already copied.
    CHECK(core->drain());
    CHECK(core->failure(failed).kind == TaskFailure::Fault && core->failure(failed).code == 1);
    CHECK(core->failure(failed).line == 11 && core->failure(failed).column == 8);
    float result = 0;
    Invocation receiver;
    CHECK(core->observe(success, scalar, receiver, &result) && result == 8);
    CHECK(core->close(good) && core->drain());
    CHECK(core->failure(good).kind == TaskFailure::None);
    CHECK(core->release(bad) && core->release(good));
    empty(*core);
  }
  for (unsigned malformed = 0; malformed < 4; ++malformed) {
    auto root = core->openRoot();
    Input input{1, 0, 0, &runs};
    auto entry = leaf;
    auto value = packet(input);
    if (malformed == 0)
      entry.revision = 2;
    if (malformed == 1)
      value.bytes = 1;
    if (malformed == 2)
      value.data = reinterpret_cast<const void *>(1);
    if (malformed == 3)
      entry.frameBytes = SIZE_MAX;
    auto rejected = core->submit(root, entry, value);
    CHECK(core->drain() && core->failure(rejected).kind == TaskFailure::AdmissionRejected);
    CHECK(core->release(root));
    empty(*core);
  }
  {
    auto root = core->openRoot();
    unsigned invoked = 0;
    Input input{1, 20, 0, &invoked};
    auto task = core->submit(root, lexicalEntry, packet(input));
    CHECK(core->drain() && invoked == 0);
    float value = 0;
    Invocation receiver;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 5);
    CHECK(core->close(root) && core->drain() && core->release(root));
    empty(*core);
  }
  for (int kind = 0; kind <= 4; ++kind) {
    auto root = core->openRoot();
    unsigned invoked = 0;
    Input input{1, 20, kind, &invoked};
    auto task = core->submit(root, stopEntry, packet(input));
    CHECK(core->drain() && invoked == 0);
    Invocation receiver;
    float value = 0;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 42);
    CHECK(core->close(root) && core->drain() && core->release(root));
    empty(*core);
  }
  // A failed nested group can be handled by its owner after settlement.
  {
    auto root = core->openRoot();
    BranchInput input{0, true, &runs};
    auto task = core->submit(root, branchEntry(), {20, &input, sizeof(input)});
    CHECK(core->drain());
    Invocation receiver;
    float value = 0;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 99.0f);
    CHECK(core->close(root) && core->drain());
    CHECK(core->failure(root).kind == TaskFailure::None && core->release(root));
  }
  // Queued cancellation never invokes the entry; a yielded task is also cancellable.
  for (bool started : {false, true}) {
    auto root = core->openRoot();
    unsigned invoked = 0;
    Input input{7.0f, 10, 0, &invoked};
    auto task = core->submit(root, leaf, packet(input));
    if (started)
      CHECK(core->step());
    CHECK(core->cancel(root, started));
    CHECK(core->drain() && core->settled(task) && invoked == (started ? 1U : 0U));
    CHECK(core->failure(root).kind ==
          (started ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    CHECK(core->failure(task).kind == TaskFailure::Cancelled && core->release(root));
  }
  // Cancellation while a parent waits joins descendants before publication.
  {
    auto root = core->openRoot();
    BranchInput input{8, false, &runs};
    auto task = core->submit(root, branchEntry(), {20, &input, sizeof(input)});
    for (unsigned i = 0; i < 5; ++i)
      CHECK(core->step());
    CHECK(core->cancel(root) && core->drain() && core->settled(task));
    CHECK(core->release(root));
    empty(*core);
  }
  // Every admission allocation position rolls back, including partially prepared frames.
  for (unsigned fail = 1; fail <= 2; ++fail) {
    core->failAllocationAt(fail);
    auto root = core->openRoot();
    Input input{1, 0, 0, &runs};
    auto task = core->submit(root, leaf, packet(input));
    CHECK(core->drain() && core->failure(task).kind == TaskFailure::Fault);
    CHECK(core->failure(task).code == 2 && core->release(root));
    empty(*core);
  }
  core->failAllocationAt(0);
  // Backing survives packet destruction and the group, including repeated observation.
  {
    Invocation parent, receiver;
    auto *values = static_cast<float *>(parent.allocate(2 * sizeof(float)));
    CHECK(values);
    values[0] = 12;
    values[1] = 34;
    unsigned destroyed = 0;
    TensorInput input{&parent, {values, values}, &destroyed};
    auto root = core->openRoot();
    auto task = core->submit(root, tensorEntry, {30, &input, sizeof(input)});
    parent.settle();
    CHECK(core->drain() && destroyed == 1);
    Tensor result{}, again{};
    CHECK(core->observe(task, tensorLayout, receiver, &result));
    CHECK(core->observe(task, tensorLayout, receiver, &again));
    CHECK(core->close(root) && core->drain() && core->release(root));
    CHECK(result.values[0] == 12 && again.values[1] == 34 && receiver.live() == 2);
    empty(*core);
  }
  // All rejection paths are bounded and leave a settled, inspectable failure handle.
  for (unsigned mode = 0; mode < 5; ++mode) {
    TaskLimits limits;
    if (mode == 0)
      limits.ready = 1;
    if (mode == 1)
      limits.inFlight = 1;
    if (mode == 2)
      limits.records = 1;
    if (mode == 3)
      limits.bytes = sizeof(Leaf) + sizeof(float);
    auto small = TaskCore::create(limits);
    auto root = small->openRoot();
    Input input{1, 0, 0, &runs};
    auto first = small->submit(root, leaf, packet(input));
    auto malformed = packet(input);
    if (mode == 4)
      malformed.layout = 999;
    auto rejected = small->submit(root, leaf, malformed);
    CHECK(rejected && small->settled(rejected));
    CHECK(small->failure(rejected).kind == TaskFailure::AdmissionRejected);
    CHECK(small->drain() && small->settled(first));
    CHECK(small->failure(root).kind == TaskFailure::AdmissionRejected);
    CHECK(small->release(root));
    empty(*small);
  }
  // Closing a successful body without observing its result is an invariant fault.
  {
    auto root = core->openRoot();
    Input input{1, 0, 0, &runs};
    core->submit(root, leaf, packet(input));
    CHECK(core->close(root) && core->drain());
    CHECK(core->failure(root).kind == TaskFailure::Fault && core->release(root));
  }
  {
    Invocation parent;
    auto *data = static_cast<float *>(parent.allocate(4));
    CHECK(data);
    unsigned destroyed = 0;
    TensorInput input{&parent, {data, data}, &destroyed};
    {
      auto dropped = TaskCore::create();
      auto root = dropped->openRoot();
      CHECK(dropped->submit(root, tensorEntry, {30, &input, sizeof(input)}));
      parent.settle();
    }
    CHECK(destroyed == 1);
  }
  empty(*core);
  std::puts("Single-worker nested progress, cached outcomes, quotas, cancellation, retained "
            "tensors, and rollback pass");
}
