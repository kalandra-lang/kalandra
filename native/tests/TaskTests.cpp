#include "../runtime/OwnedResources.h"
#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <pthread.h>
#endif
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
static void cancelledConstruction() {
  struct Input {
    std::atomic<bool> entered{false}, release{false};
    bool ownFault = false;
  };
  static const TaskEntry entry{1,
                               0x43414e43454c4354,
                               sizeof(Input),
                               alignof(Input),
                               8,
                               8,
                               [](Invocation &context, void *, const Packet &packet) {
                                 auto &input =
                                     *static_cast<Input *>(const_cast<void *>(packet.data));
                                 CHECK(context.allocate(32));
                                 input.entered.store(true, std::memory_order_release);
                                 while (!input.release.load(std::memory_order_acquire))
                                   std::this_thread::yield();
                                 if (input.ownFault)
                                   context.fault(4, 17, 3);
                                 (void)context.poll();
                                 return false;
                               },
                               [](void *) noexcept { CHECK(false); },
                               [](Task &) { CHECK(false); },
                               &scalar};
  for (bool cancel : {false, true})
    for (bool deadline : {false, true})
      for (bool ownFault : {false, true}) {
        auto core = TaskCore::create();
        CHECK(core);
        auto group = core->openRoot();
        Input input;
        input.ownFault = ownFault;
        TaskHandle task;
        std::jthread preparing([&] {
          task = core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), nullptr});
        });
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!input.entered.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < timeout)
          std::this_thread::yield();
        CHECK(input.entered.load(std::memory_order_acquire));
        if (cancel)
          CHECK(core->cancel(group, deadline));
        input.release.store(true, std::memory_order_release);
        preparing.join();
        CHECK(task && core->drain() && core->settled(task));
        const auto failure = core->failure(task);
        CHECK(failure.kind == (ownFault || !cancel ? TaskFailure::Fault : TaskFailure::Cancelled));
        if (failure.kind == TaskFailure::Fault)
          CHECK(failure.code == (ownFault ? 4 : 3));
        if (ownFault)
          CHECK(failure.line == 17 && failure.column == 3);
        if (cancel)
          CHECK(core->failure(group).kind ==
                (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
        CHECK(core->close(group) && core->drain() && core->release(group));
        const auto stats = core->stats();
        CHECK(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
              !stats.unfinished);
      }
}
// A recursive ordinary call must not acquire scheduler records or groups.
struct CallInput {
  unsigned depth;
  unsigned *destroyed;
  bool fault = false, tail = false;
};
struct CallState {
  CallInput input;
  float child = 0;
  bool yielded = false;
};
static const TaskEntry &recursiveEntry();
static const TaskEntry &mutualEntry();
static bool constructCall(Invocation &, void *frame, const Packet &packet) {
  new (frame) CallState{*static_cast<const CallInput *>(packet.data)};
  return true;
}
static void destroyCall(void *frame) noexcept {
  auto &f = *static_cast<CallState *>(frame);
  ++*f.input.destroyed;
  f.~CallState();
}
static void resumeCall(Task &task) {
  auto &f = *static_cast<CallState *>(task.frame());
  if (f.input.depth) {
    auto input = f.input;
    --input.depth;
    auto *context = &task.context();
    const auto &target = input.tail && input.depth % 2 ? mutualEntry() : recursiveEntry();
    if (!task.call(target, {40, &input, sizeof(input), context}, &f.child, {}, input.tail)) {
      // Pushing is staged: the caller's lease remains valid until resume returns.
      CHECK(&task.context() == context && task.frame() == &f);
      return;
    }
    if (!f.input.tail)
      f.child += 1;
  } else {
    if (f.input.fault) {
      task.fault(1, 9, 7);
      return;
    }
    if (!f.yielded) {
      f.yielded = true;
      task.yield();
      return;
    }
  }
  task.complete(&f.child);
}
static const TaskEntry &recursiveEntry() {
  static const TaskEntry entry{1,
                               40,
                               sizeof(CallInput),
                               alignof(CallInput),
                               sizeof(CallState),
                               alignof(CallState),
                               constructCall,
                               destroyCall,
                               resumeCall,
                               &scalar};
  return entry;
}
static const TaskEntry &mutualEntry() {
  static const TaskEntry entry{1,
                               40,
                               sizeof(CallInput),
                               alignof(CallInput),
                               sizeof(CallState) + 16,
                               alignof(CallState),
                               constructCall,
                               destroyCall,
                               resumeCall,
                               &scalar};
  return entry;
}
static void callStacks() {
  for (unsigned mode = 0; mode < 6; ++mode) {
    TaskLimits limits;
    limits.records = limits.groups = limits.ready = limits.inFlight = 1;
    limits.depth = 0;
    if (mode == 2)
      limits.callBytes = 2048;
    auto core = TaskCore::create(limits);
    CHECK(core);
    unsigned destroyed = 0;
    CallInput input{mode == 5   ? 200000U
                    : mode == 0 ? 20000U
                                : 64U,
                    &destroyed, mode == 1, mode == 5};
    if (mode == 3)
      core->failAllocationAt(3);
    auto group = core->openRoot();
    auto handle = core->submit(group, recursiveEntry(), {40, &input, sizeof(input)});
    if (mode == 4) {
      for (unsigned i = 0; i < 30; ++i)
        CHECK(core->step());
      CHECK(core->cancel(group));
    }
    CHECK(core->drainToCompletion() && core->settled(handle));
    auto stats = core->stats();
    CHECK(stats.records == 1 && stats.groups == 1);
    CHECK(stats.callFrames == 0 && stats.callBytes == 0);
    if (mode == 0 || mode == 5) {
      Invocation receiver;
      float value = 0;
      CHECK(core->observe(handle, scalar, receiver, &value));
      CHECK(value == (input.tail ? 0 : input.depth) && destroyed == input.depth + 1);
      CHECK(stats.peakCallFrames == (input.tail ? 2 : input.depth));
    } else if (mode == 4) {
      CHECK(core->failure(handle).kind == TaskFailure::Cancelled);
      CHECK(destroyed == 31);
    } else {
      CHECK(core->failure(handle).kind == TaskFailure::Fault);
      CHECK(core->failure(handle).code == (mode == 1 ? 1 : 2));
      CHECK(destroyed > 0);
    }
    CHECK(core->close(group) && core->drain() && core->release(group));
    CHECK(core->stats().retainedBytes == 0);
  }
}
// Independently owned frames resume on the caller's heap call stack. A borrowed
// root cannot be tail-replaced, and neither return nor cancellation settles it.
struct BorrowProbe {
  std::atomic<unsigned> entered{0}, left{0}, destroyed{0};
  unsigned ordinaryDestroyed = 0;
  std::atomic<unsigned> finalized{0};
};
struct BorrowState {
  Invocation execution;
  BorrowProbe *probe;
  BorrowedTaskEntry descriptor{};
  BorrowState *nested = nullptr;
  unsigned value = 0;
  float child = 0;
  std::atomic<bool> active{false};
  bool waiting = false, fault = false, dropCaller = false;
};
static bool enterBorrow(void *p, Invocation &caller) noexcept {
  auto &state = *static_cast<BorrowState *>(p);
  if (state.active.exchange(true, std::memory_order_acquire))
    return false;
  ++state.probe->entered;
  state.execution.bindCancellation(InvocationABI::cancellationToken(caller));
  if (state.dropCaller)
    caller.release(p);
  return true;
}
static void leaveBorrow(void *p) noexcept {
  auto &state = *static_cast<BorrowState *>(p);
  CHECK(state.active);
  state.execution.bindCancellation(nullptr);
  state.execution.allowNativeInterrupts(false);
  state.active.store(false, std::memory_order_release);
  ++state.probe->left;
}
static void destroyBorrow(void *p) noexcept {
  auto &state = *static_cast<BorrowState *>(p);
  CHECK(!state.active);
  ++state.probe->destroyed;
  state.~BorrowState();
}
static void resumeBorrow(Task &task) {
  auto &state = *static_cast<BorrowState *>(task.frame());
  CHECK(&task.context() == &state.execution && state.active);
  if (state.waiting) {
    task.yield();
    return;
  }
  if (state.nested) {
    auto pin = state.execution.lease(state.nested);
    CHECK(pin);
    if (!task.callBorrowed(state.nested->descriptor, std::move(*pin), &state.child))
      return;
    const float result = state.child + 1;
    task.complete(&result);
    return;
  }
  // Asking for tail lowering must preserve the borrowed root and its private
  // context even though this child's result has the same layout.
  CallInput input{8, &state.probe->ordinaryDestroyed, state.fault, true};
  if (!task.call(recursiveEntry(), {40, &input, sizeof(input), &state.execution}, &state.child, {},
                 true))
    return;
  const float result = float(++state.value) + state.child;
  task.complete(&result);
}
static const TaskEntry borrowedBody{
    1, 0, 0, 1, sizeof(BorrowState), alignof(BorrowState), nullptr, nullptr, resumeBorrow, &scalar};
struct BorrowCaller {
  BorrowState *state;
  unsigned count, done = 0;
  float item = 0, sum = 0;
};
static bool constructBorrowCaller(Invocation &ctx, void *p, const Packet &packet) {
  auto input = *static_cast<const BorrowCaller *>(packet.data);
  if (!packet.source || !ctx.retainFrom(*packet.source, input.state))
    return false;
  new (p) BorrowCaller(input);
  return true;
}
static void resumeBorrowCaller(Task &task) {
  auto &f = *static_cast<BorrowCaller *>(task.frame());
  while (f.done < f.count) {
    auto pin = task.context().lease(f.state);
    if (!task.callBorrowed(f.state->descriptor, pin ? std::move(*pin) : AllocationLease{}, &f.item))
      return;
    ++f.done;
    f.sum += f.item;
  }
  task.complete(&f.sum);
}
static const TaskEntry borrowCaller{1,
                                    70,
                                    sizeof(BorrowCaller),
                                    alignof(BorrowCaller),
                                    sizeof(BorrowCaller),
                                    alignof(BorrowCaller),
                                    constructBorrowCaller,
                                    nothing,
                                    resumeBorrowCaller,
                                    &scalar};
static bool rejectBorrowCopy(Invocation &, void *, const Invocation &, const void *) noexcept {
  return false;
}
static const ValueLayout rejectedBorrowValue{81, sizeof(float), alignof(float), rejectBorrowCopy,
                                             nothing};
static void borrowedCalls() {
  for (unsigned workers : {1u, 4u})
    for (unsigned mode = 0; mode < 8; ++mode) {
      BorrowProbe probe;
      Invocation owner;
      auto *memory = owner.allocateResource(sizeof(BorrowState), destroyBorrow);
      CHECK(memory);
      auto *state = new (memory) BorrowState;
      state->probe = &probe;
      state->descriptor = {&borrowedBody, &state->execution, state,
                           state,         enterBorrow,       leaveBorrow};
      TaskEntry invalid = borrowedBody;
      if (mode == 5)
        invalid.frameAlignment = 0;
      if (mode == 7)
        invalid.result = &rejectedBorrowValue;
      if (mode == 5 || mode == 7)
        state->descriptor.entry = &invalid;
      state->waiting = mode == 1;
      state->fault = mode == 2;
      state->dropCaller = mode == 3;
      TaskLimits limits;
      limits.workers = workers;
      limits.records = limits.groups = limits.inFlight = limits.ready = 1;
      limits.depth = 0;
      if (mode == 6)
        limits.callBytes = 1;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      BorrowCaller input{state, mode == 0 ? 3u : 1u};
      auto task = core->submit(group, borrowCaller, {70, &input, sizeof(input), &owner});
      CHECK(task);
      if (mode == 0 || mode == 3)
        owner.settle();
      if (mode == 4)
        core->failAllocationAt(1);
      if (mode == 1) {
        CHECK(core->step() && core->step());
        CHECK(probe.entered == 1 && probe.left == 0);
        CHECK(core->cancel(group));
      }
      CHECK(core->drainToCompletion() && core->settled(task));
      CHECK(core->stats().callFrames == 0 && core->stats().callBytes == 0);
      if (mode == 0 || mode == 3) {
        Invocation receiver;
        float result = 0;
        CHECK(core->observe(task, scalar, receiver, &result));
        CHECK(result == (mode == 0 ? 6 : 1));
        CHECK(probe.entered == input.count && probe.left == input.count);
        CHECK(probe.ordinaryDestroyed == 9 * input.count);
        CHECK(mode != 3 || probe.destroyed == 1); // advancing pin was the final owner
      } else {
        auto failure = core->failure(task);
        CHECK(failure.kind == (mode == 1 ? TaskFailure::Cancelled : TaskFailure::Fault));
        if (mode != 1)
          CHECK(failure.code == (mode == 2 ? 1 : mode == 4 || mode == 6 ? 2 : 3));
        CHECK(probe.entered == (mode == 4 || mode == 5 || mode == 6 ? 0 : 1) &&
              probe.left == probe.entered);
        CHECK(probe.destroyed == 0 && !state->active);
        CHECK(InvocationABI::cancellationToken(state->execution) == nullptr);
        CHECK(mode == 2 || mode == 7 || state->execution.usable());
        if (mode == 7)
          CHECK(state->execution.status() == 3);
      }
      CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
      owner.settle();
      CHECK(probe.destroyed == 1 && !core->stats().retainedBytes && !core->stats().callBytes);
    }
}

static void deepBorrowedCalls() {
  constexpr unsigned depth = 10000;
  Invocation owner;
  BorrowProbe probe;
  std::vector<BorrowState *> states;
  for (unsigned i = 0; i <= depth; ++i) {
    auto *memory = owner.allocateResource(sizeof(BorrowState), destroyBorrow);
    CHECK(memory);
    auto *state = new (memory) BorrowState;
    state->probe = &probe;
    state->descriptor = {&borrowedBody, &state->execution, state, state, enterBorrow, leaveBorrow};
    if (!states.empty()) {
      state->nested = states.back();
      CHECK(state->execution.retainFrom(owner, state->nested));
    }
    states.push_back(state);
  }
  TaskLimits limits;
  limits.records = limits.groups = limits.ready = limits.inFlight = 1;
  limits.depth = 0;
  auto core = TaskCore::create(limits);
  CHECK(core);
  auto group = core->openRoot();
  BorrowCaller input{states.back(), 1};
  auto task = core->submit(group, borrowCaller, {70, &input, sizeof(input), &owner});
  CHECK(task && core->drainToCompletion() && core->settled(task));
  Invocation result;
  float value = 0;
  CHECK(core->observe(task, scalar, result, &value) && value == depth + 1);
  CHECK(probe.entered == depth + 1 && probe.left == depth + 1 && probe.destroyed == 0);
  CHECK(core->stats().peakCallFrames == depth + 3 && core->stats().callFrames == 0);
  CHECK(core->stats().callBytes == 0 && core->stats().records == 1 && core->stats().groups == 1);
  CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
  // Keep anchor storage alive while explicitly settling the independent ledgers.
  for (auto *state : states)
    state->execution.settle();
  owner.settle();
  CHECK(probe.destroyed == depth + 1);
}
static void overlappingBorrowedCalls() {
  Invocation owner;
  BorrowProbe probe;
  auto *state = new (owner.allocateResource(sizeof(BorrowState), destroyBorrow)) BorrowState;
  state->probe = &probe;
  state->descriptor = {&borrowedBody, &state->execution, state, state, enterBorrow, leaveBorrow};
  state->waiting = true;
  auto core = TaskCore::create();
  CHECK(core);
  auto first = core->openRoot(), second = core->openRoot();
  BorrowCaller input{state, 1};
  auto a = core->submit(first, borrowCaller, {70, &input, sizeof(input), &owner});
  CHECK(a && core->step() && core->step() && probe.entered == 1);
  auto b = core->submit(second, borrowCaller, {70, &input, sizeof(input), &owner});
  CHECK(b);
  for (unsigned i = 0; i < 1000 && !core->settled(b); ++i)
    core->step();
  CHECK(core->settled(b) && core->failure(b).kind == TaskFailure::Fault &&
        core->failure(b).code == 3);
  CHECK(probe.entered == 1 && probe.left == 0 && !state->execution.poll());
  CHECK(core->cancel(first) && core->drainToCompletion());
  CHECK(core->failure(a).kind == TaskFailure::Cancelled && probe.left == 1);
  CHECK(core->close(first) && core->close(second) && core->drainToCompletion());
  CHECK(core->release(first) && core->release(second));
  owner.settle();
  CHECK(probe.destroyed == 1 && !core->stats().retainedBytes && !core->stats().callBytes);
}
struct GroupProbe {
  std::atomic<unsigned> steps{0}, constructed{0}, destroyed{0};
  bool external = false;
  ExternalCompletion completion;
};
struct GroupChild {
  GroupProbe *probe;
};
static bool constructGroupChild(Invocation &, void *p, const Packet &packet) {
  auto *probe = *static_cast<GroupProbe *const *>(packet.data);
  new (p) GroupChild{probe};
  ++probe->constructed;
  return true;
}
static void destroyGroupChild(void *p) noexcept {
  auto &f = *static_cast<GroupChild *>(p);
  ++f.probe->destroyed;
  f.~GroupChild();
}
static void resumeGroupChild(Task &task) {
  auto &probe = *static_cast<GroupChild *>(task.frame())->probe;
  ++probe.steps;
  if (probe.external) {
    probe.completion = task.suspendExternal();
    CHECK(probe.completion);
  } else {
    task.yield();
  }
}
static const TaskEntry groupChild{1,
                                  84,
                                  sizeof(GroupProbe *),
                                  alignof(GroupProbe *),
                                  sizeof(GroupChild),
                                  alignof(GroupChild),
                                  constructGroupChild,
                                  destroyGroupChild,
                                  resumeGroupChild,
                                  &scalar};
static void destroyProducer(void *) noexcept;
struct PersistentProducer : OwnedState {
  PersistentProducer()
      : OwnedState(OwnedKind::Generator, sizeof(PersistentProducer), destroyProducer, nothing) {}
  Invocation execution;
  BorrowedGroupOwner groups{execution};
  BorrowedCleanup retirement{};
  BorrowedTaskEntry descriptor{};
  GroupProbe *child;
  BorrowProbe *probe;
  GroupHandle group;
  TaskHandle task;
  bool active = false, started = false, closed = false, failDuringAdvance = false;
};
static bool enterProducer(void *p, Invocation &caller) noexcept {
  auto &f = *static_cast<PersistentProducer *>(p);
  CHECK(!f.active);
  f.active = true;
  ++f.probe->entered;
  f.execution.bindCancellation(InvocationABI::cancellationToken(caller));
  return true;
}
static void leaveProducer(void *p) noexcept {
  auto &f = *static_cast<PersistentProducer *>(p);
  CHECK(f.active);
  f.execution.bindCancellation(nullptr);
  f.execution.allowNativeInterrupts(false);
  f.active = false;
  ++f.probe->left;
}
static void destroyProducer(void *p) noexcept {
  auto &f = *static_cast<PersistentProducer *>(p);
  CHECK(!f.active && f.child->destroyed == f.child->constructed);
  ++f.probe->destroyed;
  f.~PersistentProducer();
}
static void resumeProducer(Task &task) {
  auto &f = *static_cast<PersistentProducer *>(task.frame());
  if (!f.started) {
    f.group = task.openGroup();
    if (!f.group)
      return;
    auto *input = f.child;
    f.task = task.spawn(f.group, groupChild, {84, &input, sizeof(input)});
    CHECK(f.task);
    if (f.closed)
      CHECK(task.close(f.group));
    f.started = true;
    if (f.failDuringAdvance) {
      task.yield();
      return;
    }
    const float value = 42;
    task.complete(&value);
    return;
  }
  if (f.failDuringAdvance) {
    task.fault(2, 15, 6);
    return;
  }
  if (!task.settleBorrowedGroups())
    return;
  const float value = 0;
  task.complete(&value);
}
static const TaskEntry persistentBody{1,
                                      0,
                                      0,
                                      1,
                                      sizeof(PersistentProducer),
                                      alignof(PersistentProducer),
                                      nullptr,
                                      nullptr,
                                      resumeProducer,
                                      &scalar};
struct ProducerCaller {
  PersistentProducer *producer;
  float value = 0;
};
static bool constructProducerCaller(Invocation &ctx, void *p, const Packet &packet) {
  auto input = *static_cast<const ProducerCaller *>(packet.data);
  if (!packet.source || !ctx.retainFrom(*packet.source, input.producer))
    return false;
  new (p) ProducerCaller(input);
  return true;
}
static void resumeProducerCaller(Task &task) {
  auto &f = *static_cast<ProducerCaller *>(task.frame());
  auto pin = task.context().lease(f.producer);
  CHECK(pin);
  if (!task.callBorrowed(f.producer->descriptor, std::move(*pin), &f.value))
    return;
  task.complete(&f.value);
}
static const TaskEntry producerCaller{1,
                                      85,
                                      sizeof(ProducerCaller),
                                      alignof(ProducerCaller),
                                      sizeof(ProducerCaller),
                                      alignof(ProducerCaller),
                                      constructProducerCaller,
                                      nothing,
                                      resumeProducerCaller,
                                      &scalar};
static void persistentGroups() {
  for (unsigned workers : {1u, 4u})
    for (unsigned mode = 0; mode < 9; ++mode) {
      Invocation owner;
      BorrowProbe probe;
      GroupProbe child;
      child.external = mode == 5 || mode == 6;
      auto *memory = owner.allocateResource(sizeof(PersistentProducer), OwnedState::cleanup, true);
      CHECK(memory);
      auto *state = new (memory) PersistentProducer;
      CHECK(owner.trackResource(state));
      state->execution.bindResourceBudget(owner, state);
      CHECK(!state->execution.poll());
      state->child = &child;
      state->probe = &probe;
      state->closed = mode != 0;
      state->descriptor = {&persistentBody, &state->execution, state,         state,
                           enterProducer,   leaveProducer,     &state->groups};
      TaskEntry failing = persistentBody;
      if (mode == 3) {
        failing.result = &rejectedBorrowValue;
        state->descriptor.entry = &failing;
      }
      TaskLimits limits;
      limits.workers = workers;
      limits.records = limits.groups = limits.ready = limits.inFlight = 4;
      limits.depth = 1;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto root = core->openRoot();
      ProducerCaller input{state};
      auto task = core->submit(root, producerCaller, {85, &input, sizeof(input), &owner});
      CHECK(task);
      if (mode == 2) {
        CHECK(core->step() && core->step());
        // The item is ready, but publication to the caller is still pending.
        CHECK(state->started && probe.left == 0);
        CHECK(core->cancel(root));
        CHECK(core->drainToCompletion() && core->settled(task));
      } else {
        for (unsigned i = 0; i < 1000 && !core->settled(task); ++i)
          core->step();
        CHECK(core->settled(task));
      }
      CHECK(InvocationABI::cancellationToken(state->execution) == nullptr);
      if (mode == 2 || mode == 3) {
        CHECK(core->failure(task).kind ==
              (mode == 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
        CHECK(child.destroyed == 1 && core->stats().unfinished == 0);
      } else {
        Invocation result;
        float value = 0;
        CHECK(core->observe(task, scalar, result, &value) && value == 42);
        CHECK(child.destroyed == 0 && core->stats().unfinished == 1);
        if (child.external) {
          CHECK(child.completion && core->stats().externalPending == 1);
        } else {
          const auto steps = child.steps.load();
          CHECK(!core->drain(64) && child.steps > steps);
        }
        // Detached groups belong to the producer, never the host's owner0.
        CHECK(!core->close(state->group) && !core->cancel(state->group) &&
              !core->release(state->group));
        Invocation denied;
        CHECK(!core->observe(state->task, scalar, denied, &value) && denied.status() == 3);
      }
      CHECK(core->close(root));
      for (unsigned i = 0; i < 1000 && !core->settled(root); ++i)
        core->step();
      CHECK(core->settled(root) && core->release(root));
      CHECK(probe.destroyed == 0 && probe.entered == 1 && probe.left == 1);
      if (mode == 7) {
        auto other = TaskCore::create();
        CHECK(other);
        auto outside = other->openRoot();
        auto invalid = other->submit(outside, producerCaller, {85, &input, sizeof(input), &owner});
        CHECK(invalid && other->drainToCompletion() && other->settled(invalid));
        CHECK(other->failure(invalid).kind == TaskFailure::Fault &&
              other->failure(invalid).code == 3);
        CHECK(!state->execution.poll() && child.destroyed == 0);
        CHECK(other->close(outside) && other->drainToCompletion() && other->release(outside));
      }
      if (mode < 2 || mode >= 4) {
        const auto before = core->stats().recursiveBytes;
        CHECK(state->execution.allocate(4096));
        CHECK(core->stats().recursiveBytes >= before + 4096);
        if (mode == 4)
          CHECK(!state->execution.allocate(1024 * 1024) && state->execution.status() == 2);
        auto cleanup = core->openRoot();
        auto closing = core->submit(cleanup, producerCaller, {85, &input, sizeof(input), &owner});
        CHECK(closing);
        if (child.external) {
          for (unsigned i = 0; i < 32; ++i)
            core->step();
          CHECK(!core->settled(closing) && child.destroyed == 0 &&
                core->stats().externalPending == 1);
          if (mode == 6) {
            CHECK(core->cancel(cleanup));
            for (unsigned i = 0; i < 32; ++i)
              core->step();
            CHECK(!core->settled(closing) && child.destroyed == 0);
          }
          CHECK(child.completion.signal());
        }
        CHECK(core->drainToCompletion() && core->settled(closing));
        Invocation result;
        float value = -1;
        if (mode == 6)
          CHECK(core->failure(closing).kind == TaskFailure::Cancelled);
        else if (mode == 4)
          CHECK(core->failure(closing).kind == TaskFailure::Fault &&
                core->failure(closing).code == 2);
        else
          CHECK(core->observe(closing, scalar, result, &value) && value == 0);
        CHECK(child.destroyed == 1 && probe.entered == (mode == 7 ? 3 : 2) &&
              probe.left == probe.entered);
        CHECK(core->close(cleanup) && core->drainToCompletion() && core->release(cleanup));
      }
      if (mode == 8) {
        // Header metadata outlives settled producer locals. Reject another
        // advance without reading a binding freed with the body ledger.
        state->execution.settle();
        auto rejectedRoot = core->openRoot();
        auto rejected =
            core->submit(rejectedRoot, producerCaller, {85, &input, sizeof(input), &owner});
        CHECK(rejected && core->drainToCompletion() && core->settled(rejected));
        CHECK(core->failure(rejected).kind == TaskFailure::Fault &&
              core->failure(rejected).code == 3);
        CHECK(core->close(rejectedRoot) && core->drainToCompletion() &&
              core->release(rejectedRoot));
        CHECK(probe.entered == 3 && probe.left == 3);
      }
      owner.settle();
      CHECK(probe.destroyed == 1 && !core->stats().groups && !core->stats().records &&
            !core->stats().retainedBytes && !core->stats().callBytes);
    }
}

static void persistentGroupAdmission() {
  for (unsigned mode = 0; mode < 16; ++mode) {
    Invocation owner;
    BorrowProbe probe;
    GroupProbe child;
    auto *memory = owner.allocateResource(sizeof(PersistentProducer), OwnedState::cleanup, true);
    CHECK(memory);
    auto *state = new (memory) PersistentProducer;
    CHECK(owner.trackResource(state));
    state->execution.bindResourceBudget(owner, state);
    state->child = &child;
    state->probe = &probe;
    state->descriptor = {&persistentBody, &state->execution, state,         state,
                         enterProducer,   leaveProducer,     &state->groups};
    TaskLimits limits;
    limits.records = limits.groups = limits.ready = limits.inFlight = 4;
    limits.depth = 1;
    if (mode < 8)
      limits.bytes = 512 + mode * 512;
    auto core = TaskCore::create(limits);
    CHECK(core);
    if (mode >= 8)
      core->failAllocationAt(mode - 7);
    auto root = core->openRoot();
    ProducerCaller input{state};
    auto task = core->submit(root, producerCaller, {85, &input, sizeof(input), &owner});
    CHECK(task);
    for (unsigned i = 0; i < 1000 && !core->settled(task); ++i)
      core->step();
    CHECK(core->settled(task));
    const bool success = core->failure(task).kind == TaskFailure::None;
    if (success) {
      Invocation result;
      float value = 0;
      CHECK(core->observe(task, scalar, result, &value) && value == 42);
    }
    CHECK(core->close(root));
    for (unsigned i = 0; i < 1000 && !core->settled(root); ++i)
      core->step();
    CHECK(core->settled(root) && core->release(root));
    core->failAllocationAt(0);
    if (success) {
      auto closingRoot = core->openRoot();
      auto closing = core->submit(closingRoot, producerCaller, {85, &input, sizeof(input), &owner});
      CHECK(closing);
      for (unsigned i = 0; i < 1000 && !core->settled(closing); ++i)
        core->step();
      CHECK(core->settled(closing));
      // A very tight limit may reject the additional cleanup call's metadata.
      // It must nevertheless settle any group it acquired before publication.
      if (core->failure(closing).kind == TaskFailure::None) {
        Invocation result;
        float value = -1;
        CHECK(core->observe(closing, scalar, result, &value) && value == 0);
      }
      CHECK(core->close(closingRoot));
      for (unsigned i = 0; i < 1000 && !core->settled(closingRoot); ++i)
        core->step();
      CHECK(core->settled(closingRoot) && core->release(closingRoot));
    }
    CHECK(core->stats().peakBytes <= limits.bytes);
    // A rejected cleanup call never acquired this externally owned cursor.
    // Application shutdown still cancels and joins any detached producer work.
    core.reset();
    CHECK(child.destroyed == child.constructed);
    owner.settle();
    CHECK(probe.destroyed == 1);
  }
}

static bool copyIndependentString(Invocation &to, void *out, const Invocation &from,
                                  const void *in) noexcept {
  const auto &value = *static_cast<const StringDescriptor *>(in);
  if (!kalandra_string_retain_v1(&to, &from, &value))
    return false;
  *static_cast<StringDescriptor *>(out) = value;
  return true;
}
static const ValueLayout independentString{93, sizeof(StringDescriptor), alignof(StringDescriptor),
                                           copyIndependentString, nothing};
struct IndependentOutcome {
  Invocation *source;
  StringDescriptor text;
};
static bool constructIndependentOutcome(Invocation &, void *out, const Packet &packet) {
  new (out) IndependentOutcome(*static_cast<const IndependentOutcome *>(packet.data));
  return true;
}
static void resumeIndependentOutcome(Task &task) {
  auto &f = *static_cast<IndependentOutcome *>(task.frame());
  // The body ledger can be settled before its saved, managed terminal result
  // is copied to the task outcome. The result's own ledger remains readable.
  task.context().settle();
  task.completeFrom(*f.source, &f.text);
}
static const TaskEntry independentOutcome{1,
                                          93,
                                          sizeof(IndependentOutcome),
                                          alignof(IndependentOutcome),
                                          sizeof(IndependentOutcome),
                                          alignof(IndependentOutcome),
                                          constructIndependentOutcome,
                                          nothing,
                                          resumeIndependentOutcome,
                                          &independentString};
static void independentOutcomes() {
  for (unsigned workers : {1u, 4u}) {
    Invocation source;
    StringDescriptor text;
    kalandra_string_literal_v1(&source, &text, "terminal", 8);
    CHECK(!source.poll() && text.allocated);
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    IndependentOutcome input{&source, text};
    auto task = core->submit(group, independentOutcome, {93, &input, sizeof(input)});
    CHECK(task && core->drainToCompletion() && core->settled(task));
    source.settle();
    Invocation receiver;
    StringDescriptor result;
    CHECK(core->observe(task, independentString, receiver, &result));
    CHECK(result.byteLength == 8 && std::string_view(result.data, 8) == "terminal");
    CHECK(receiver.lease(result.allocated));
    CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
    CHECK(core->stats().retainedBytes == 0 && core->stats().retainedOwners == 0);
  }
}

static Failure producerCleanupFailure(void *p) noexcept {
  auto &f = *static_cast<PersistentProducer *>(p);
  return f.execution.status() ? Failure{TaskFailure::Fault, f.execution.status(),
                                        f.execution.line(), f.execution.column()}
                              : Failure{};
}
static void finishProducerCleanup(void *p) noexcept {
  auto &f = *static_cast<PersistentProducer *>(p);
  CHECK(f.child->destroyed == f.child->constructed);
  f.execution.settle();
  ++f.probe->finalized;
}
struct CleanupCaller {
  PersistentProducer *producer;
  bool requested = false;
  bool scoped = false;
  unsigned abandon = 0;
  ResourceControl *scope = nullptr;
};
static bool constructCleanupCaller(Invocation &ctx, void *p, const Packet &packet) {
  auto input = *static_cast<const CleanupCaller *>(packet.data);
  if (!packet.source || !ctx.retainFrom(*packet.source, input.producer))
    return false;
  new (p) CleanupCaller(input);
  return true;
}
static void resumeCleanupCaller(Task &task) {
  auto &f = *static_cast<CleanupCaller *>(task.frame());
  if (f.scoped) {
    if (!f.scope) {
      auto descriptor = ownedDescriptor(f.producer);
      const OwnedDescriptor *values[] = {&descriptor};
      f.scope =
          static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&task.context(), values, 1));
      CHECK(f.scope);
    }
    if (f.abandon) {
      if (f.abandon == 1) {
        const float value = 0;
        task.complete(&value);
      } else if (f.abandon == 2)
        task.fault(7, 9, 4);
      else if (f.abandon == 3)
        task.suspendUntil(std::chrono::steady_clock::now() + std::chrono::hours(1));
      else {
        float value = 0;
        auto pin = task.context().lease(f.producer);
        CHECK(pin);
        if (!task.callBorrowed(f.producer->descriptor, std::move(*pin), &value))
          return;
        CHECK(false); // the borrowed body deliberately fails before an item
      }
      return;
    }
    if (!task.leaveOwnedScope(f.scope))
      return;
    CHECK(f.producer->groups.cleanupFinished());
    f.scope = nullptr;
    if (auto failure = task.ownedCleanupFailure(); failure.kind != TaskFailure::None) {
      task.fail(failure);
      return;
    }
    const float value = 0;
    task.complete(&value);
    return;
  }
  if (!f.requested) {
    auto pin = task.context().lease(f.producer);
    CHECK(pin);
    f.requested = true;
    auto started = task.beginOwnedCleanup(
        f.producer->groups, std::move(*pin),
        {f.producer, nullptr, finishProducerCleanup, producerCleanupFailure});
    if (started != CleanupStart::Complete)
      return;
  }
  CHECK(f.producer->groups.cleanupFinished());
  if (auto failure = task.ownedCleanupFailure(); failure.kind != TaskFailure::None) {
    task.fail(failure);
    return;
  }
  const float value = 0;
  task.complete(&value);
}

static const TaskEntry cleanupCaller{1,
                                     94,
                                     sizeof(CleanupCaller),
                                     alignof(CleanupCaller),
                                     sizeof(CleanupCaller),
                                     alignof(CleanupCaller),
                                     constructCleanupCaller,
                                     nothing,
                                     resumeCleanupCaller,
                                     &scalar};
static void failedAdvanceCleanup() {
  for (unsigned workers : {1u, 4u}) {
    Invocation owner;
    BorrowProbe probe;
    GroupProbe child;
    child.external = true;
    auto *memory = owner.allocateResource(sizeof(PersistentProducer), OwnedState::cleanup, true);
    CHECK(memory);
    auto *state = new (memory) PersistentProducer;
    CHECK(owner.trackResource(state));
    state->execution.bindResourceBudget(owner, state);
    state->child = &child;
    state->probe = &probe;
    state->failDuringAdvance = true;
    state->retirement = {state, nullptr, finishProducerCleanup, producerCleanupFailure};
    state->deferredOwner = &state->groups;
    state->deferredCleanup = &state->retirement;
    state->descriptor = {&persistentBody, &state->execution, state,         state,
                         enterProducer,   leaveProducer,     &state->groups};
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto root = core->openRoot();
    CleanupCaller input{state};
    input.scoped = true;
    input.abandon = 4;
    auto task = core->submit(root, cleanupCaller, {94, &input, sizeof(input), &owner});
    CHECK(task);
    for (unsigned i = 0; i < 64; ++i)
      core->step();
    CHECK(!core->settled(task) && probe.finalized == 0 && state->active);
    CHECK(child.completion.signal());
    CHECK(core->drainToCompletion() && core->settled(task));
    CHECK(probe.finalized == 1 && probe.entered == probe.left && !state->active);
    const auto failure = core->failure(task);
    CHECK(failure.kind == TaskFailure::Fault && failure.code == 2 && failure.line == 15 &&
          failure.column == 6);
    CHECK(core->close(root) && core->drainToCompletion() && core->release(root));
    owner.settle();
    CHECK(probe.destroyed == 1 && core->stats().retainedBytes == 0);
  }
}
static void ownedCleanupBatches() {
  for (unsigned workers : {1u, 4u})
    for (unsigned mode = 0; mode < 15; ++mode) {
      Invocation owner;
      BorrowProbe probe;
      GroupProbe child;
      child.external = mode % 3 == 1;
      auto *memory = owner.allocateResource(sizeof(PersistentProducer), OwnedState::cleanup, true);
      CHECK(memory);
      auto *state = new (memory) PersistentProducer;
      CHECK(owner.trackResource(state));
      state->execution.bindResourceBudget(owner, state);
      state->child = &child;
      state->probe = &probe;
      if (mode >= 3) {
        state->retirement = {state, nullptr, finishProducerCleanup, producerCleanupFailure};
        state->deferredOwner = &state->groups;
        state->deferredCleanup = &state->retirement;
      }
      state->descriptor = {&persistentBody, &state->execution, state,         state,
                           enterProducer,   leaveProducer,     &state->groups};
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto root = core->openRoot();
      ProducerCaller input{state};
      auto created = core->submit(root, producerCaller, {85, &input, sizeof(input), &owner});
      for (unsigned i = 0; i < 1000 && !core->settled(created); ++i)
        core->step();
      CHECK(core->settled(created));
      Invocation receiver;
      float value = 0;
      CHECK(core->observe(created, scalar, receiver, &value) && value == 42);
      CHECK(core->close(root));
      for (unsigned i = 0; i < 1000 && !core->settled(root); ++i)
        core->step();
      CHECK(core->release(root) && probe.finalized == 0 && child.destroyed == 0);
      if (mode % 3 == 2)
        state->execution.fault(2, 12, 8);
      auto closingRoot = core->openRoot();
      CleanupCaller closingInput{state};
      closingInput.scoped = mode >= 3;
      closingInput.abandon = mode < 6 ? 0 : mode / 3 - 1;
      auto closing = core->submit(closingRoot, cleanupCaller,
                                  {94, &closingInput, sizeof(closingInput), &owner});
      CHECK(closing);
      if (mode >= 12) {
        for (unsigned i = 0; i < 64; ++i)
          core->step();
        CHECK(probe.finalized == 0 && !core->settled(closing));
        CHECK(core->cancel(closingRoot));
      }
      if (child.external) {
        for (unsigned i = 0; i < 64; ++i)
          core->step();
        CHECK(!core->settled(closing) && probe.finalized == 0 && child.destroyed == 0);
        CHECK(child.completion.signal());
      }
      CHECK(core->drainToCompletion() && core->settled(closing));
      CHECK(probe.finalized == 1 && child.destroyed == 1 && state->execution.live() == 0);
      if (mode >= 9 && mode < 12)
        CHECK(core->failure(closing).kind == TaskFailure::Fault &&
              core->failure(closing).code == 7);
      else if (mode % 3 == 2)
        CHECK(core->failure(closing).kind == TaskFailure::Fault &&
              core->failure(closing).code == 2);
      else if (mode >= 12)
        CHECK(core->failure(closing).kind == TaskFailure::Cancelled);
      else
        CHECK(core->observe(closing, scalar, receiver, &value) && value == 0);
      CHECK(core->close(closingRoot) && core->drainToCompletion() && core->release(closingRoot));
      owner.settle();
      CHECK(probe.destroyed == 1 && core->stats().retainedBytes == 0 &&
            core->stats().records == 0 && core->stats().groups == 0);
    }
}

struct CleanupTreeProbe {
  std::atomic<unsigned> finished{0}, destroyed{0};
};
static void destroyCleanupTree(void *) noexcept;
struct CleanupTree : OwnedState {
  CleanupTree()
      : OwnedState(OwnedKind::Generator, sizeof(CleanupTree), destroyCleanupTree, nothing) {}
  Invocation storage;
  BorrowedGroupOwner owner{storage};
  BorrowedCleanup retirement{};
  CleanupTree *child = nullptr;
  CleanupTreeProbe *probe = nullptr;
  bool finished = false, scopedChildren = false;
};
static void destroyCleanupTree(void *p) noexcept {
  auto &node = *static_cast<CleanupTree *>(p);
  CHECK(node.finished);
  ++node.probe->destroyed;
  node.~CleanupTree();
}
static void finishCleanupTree(void *p) noexcept {
  auto &node = *static_cast<CleanupTree *>(p);
  CHECK(!node.finished && (!node.child || node.child->finished));
  node.storage.settle();
  node.finished = true;
  ++node.probe->finished;
}
static void childrenCleanupTree(BorrowedGroupOwner &owner, void *p) noexcept {
  auto &node = *static_cast<CleanupTree *>(p);
  if (node.scopedChildren) {
    CHECK(owner.enqueueOwnedScopeChildren(node.storage));
    return;
  }
  if (node.child) {
    auto pin = node.storage.lease(node.child);
    CHECK(pin);
    CHECK(owner.enqueueCleanupChild(node.child->owner, std::move(*pin),
                                    {node.child, childrenCleanupTree, finishCleanupTree, nullptr}));
  }
}
struct CleanupTreeCaller {
  CleanupTree *tree;
  bool requested = false;
};
static bool constructCleanupTreeCaller(Invocation &ctx, void *out, const Packet &packet) {
  const auto input = *static_cast<const CleanupTreeCaller *>(packet.data);
  if (!packet.source || !ctx.retainFrom(*packet.source, input.tree))
    return false;
  new (out) CleanupTreeCaller(input);
  return true;
}
static void resumeCleanupTreeCaller(Task &task) {
  auto &f = *static_cast<CleanupTreeCaller *>(task.frame());
  if (!f.requested) {
    f.requested = true;
    auto pin = task.context().lease(f.tree);
    CHECK(pin);
    auto started = task.beginOwnedCleanup(
        f.tree->owner, std::move(*pin), {f.tree, childrenCleanupTree, finishCleanupTree, nullptr});
    CHECK(started != CleanupStart::Rejected);
    if (started == CleanupStart::Pending)
      return;
  }
  CHECK(f.tree->finished);
  const float value = 42;
  task.complete(&value);
}
static const TaskEntry cleanupTreeCaller{1,
                                         96,
                                         sizeof(CleanupTreeCaller),
                                         alignof(CleanupTreeCaller),
                                         sizeof(CleanupTreeCaller),
                                         alignof(CleanupTreeCaller),
                                         constructCleanupTreeCaller,
                                         nothing,
                                         resumeCleanupTreeCaller,
                                         &scalar};
struct ScopeBatchCaller {
  CleanupTree *nodes[3];
  bool escape = false;
  unsigned phase = 0;
  ResourceControl *outer = nullptr, *inner = nullptr;
};
static bool constructScopeBatch(Invocation &ctx, void *out, const Packet &packet) {
  const auto input = *static_cast<const ScopeBatchCaller *>(packet.data);
  for (auto *node : input.nodes)
    if (!packet.source || !ctx.retainFrom(*packet.source, node))
      return false;
  new (out) ScopeBatchCaller(input);
  return true;
}
static void resumeScopeBatch(Task &task) {
  auto &f = *static_cast<ScopeBatchCaller *>(task.frame());
  if (!f.phase) {
    f.outer =
        static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&task.context(), nullptr, 0));
    CHECK(f.outer);
    OwnedDescriptor values[3];
    const OwnedDescriptor *pointers[3];
    for (unsigned i = 0; i < 3; ++i) {
      values[i] = ownedDescriptor(f.nodes[i]);
      pointers[i] = &values[i];
    }
    f.inner =
        static_cast<ResourceControl *>(kalandra_file_scope_enter_v1(&task.context(), pointers, 3));
    CHECK(f.inner);
    f.phase = 1;
  }
  if (f.phase == 1) {
    auto escaping = ownedDescriptor(f.nodes[2]);
    if (!task.leaveOwnedScope(f.inner, f.escape ? &escaping : nullptr))
      return;
    CHECK(f.nodes[0]->finished && f.nodes[1]->finished);
    CHECK(f.nodes[2]->finished == !f.escape);
    f.inner = nullptr;
    f.phase = 2;
  }
  if (!task.leaveOwnedScope(f.outer))
    return;
  for (auto *node : f.nodes)
    CHECK(node->finished);
  f.outer = nullptr;
  CHECK(task.ownedCleanupFailure().kind == TaskFailure::None);
  const float value = 42;
  task.complete(&value);
}
static const TaskEntry scopeBatchCaller{1,
                                        97,
                                        sizeof(ScopeBatchCaller),
                                        alignof(ScopeBatchCaller),
                                        sizeof(ScopeBatchCaller),
                                        alignof(ScopeBatchCaller),
                                        constructScopeBatch,
                                        nothing,
                                        resumeScopeBatch,
                                        &scalar};
static void scopedOwnedCleanup() {
  for (unsigned workers : {1u, 4u})
    for (bool escape : {false, true}) {
      Invocation source;
      CleanupTreeProbe probe;
      ScopeBatchCaller input{};
      input.escape = escape;
      for (auto *&node : input.nodes) {
        auto *memory = source.allocateResource(sizeof(CleanupTree), OwnedState::cleanup, true);
        CHECK(memory);
        node = new (memory) CleanupTree;
        node->probe = &probe;
        CHECK(source.trackResource(node));
        node->storage.bindResourceBudget(source, node);
        node->retirement = {node, childrenCleanupTree, finishCleanupTree, nullptr};
        node->deferredOwner = &node->owner;
        node->deferredCleanup = &node->retirement;
      }
      TaskLimits limits;
      limits.workers = workers;
      limits.records = limits.groups = limits.ready = limits.inFlight = 1;
      limits.depth = 0;
      limits.callBytes = 1;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      auto task = core->submit(group, scopeBatchCaller, {97, &input, sizeof(input), &source});
      CHECK(task && core->drainToCompletion() && core->settled(task));
      CHECK(probe.finished == 3 && core->stats().peakCallFrames == 0);
      Invocation receiver;
      float value = 0;
      CHECK(core->observe(task, scalar, receiver, &value) && value == 42);
      CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
      source.settle();
      CHECK(probe.destroyed == 3 && core->stats().retainedBytes == 0);
    }
}
static void deepOwnedCleanup() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    const unsigned workers = mode % 2 ? 4 : 1;
    const bool scoped = mode >= 2;
    constexpr unsigned depth = 10000;
    Invocation source;
    CleanupTreeProbe probe;
    std::vector<CleanupTree *> nodes;
    for (unsigned i = 0; i <= depth; ++i) {
      auto *memory = source.allocateResource(sizeof(CleanupTree), OwnedState::cleanup, true);
      CHECK(memory);
      auto *node = new (memory) CleanupTree;
      node->probe = &probe;
      node->scopedChildren = scoped;
      CHECK(source.trackResource(node));
      node->storage.bindResourceBudget(source, node);
      node->retirement = {node, childrenCleanupTree, finishCleanupTree, nullptr};
      node->deferredOwner = &node->owner;
      node->deferredCleanup = &node->retirement;
      if (!nodes.empty()) {
        node->child = nodes.back();
        if (scoped) {
          auto *control = static_cast<ResourceControl *>(
              kalandra_file_scope_enter_v1(&node->storage, nullptr, 0));
          CHECK(control);
          // Owned captures are retained directly in their logical store. An
          // additional temporary-store observer would conservatively bill the
          // same dynamic child through two paths at every nesting level.
          Invocation::OwnershipSelection selected(control->storage);
          CHECK(node->storage.retainFrom(source, node->child));
          adopt(node->storage, *node->child, control->scope);
          CHECK(!node->storage.status());
        } else
          CHECK(node->storage.retainFrom(source, node->child));
      }
      nodes.push_back(node);
    }
    TaskLimits limits;
    limits.workers = workers;
    limits.records = limits.groups = limits.ready = limits.inFlight = 1;
    limits.depth = 0;
    limits.callBytes = 1; // retirement must not allocate ordinary call frames
    limits.bytes = 128 * 1024 * 1024;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    CleanupTreeCaller input{nodes.back()};
    auto task = core->submit(group, cleanupTreeCaller, {96, &input, sizeof(input), &source});
    CHECK(task && core->drainToCompletion() && core->settled(task));
    CHECK(probe.finished == depth + 1 && core->stats().callFrames == 0 &&
          core->stats().peakCallFrames == 0 && core->stats().records == 1 &&
          core->stats().groups == 1);
    Invocation receiver;
    float value = 0;
    CHECK(core->observe(task, scalar, receiver, &value) && value == 42);
    CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
    source.settle();
    CHECK(probe.destroyed == depth + 1 && core->stats().retainedBytes == 0);
  }
}

int main() {
  failedAdvanceCleanup();
  scopedOwnedCleanup();
  ownedCleanupBatches();
  independentOutcomes();
  persistentGroupAdmission();
  persistentGroups();
  borrowedCalls();
  overlappingBorrowedCalls();
#ifndef _WIN32
  pthread_attr_t attributes;
  CHECK(pthread_attr_init(&attributes) == 0);
  CHECK(pthread_attr_setstacksize(&attributes, 512 * 1024) == 0);
  pthread_t smallStack;
  CHECK(pthread_create(
            &smallStack, &attributes,
            +[](void *) -> void * {
              callStacks();
              deepBorrowedCalls();
              deepOwnedCleanup();
              return nullptr;
            },
            nullptr) == 0);
  CHECK(pthread_attr_destroy(&attributes) == 0);
  CHECK(pthread_join(smallStack, nullptr) == 0);
#else
  callStacks();
  deepBorrowedCalls();
  deepOwnedCleanup();
#endif
  cancelledConstruction();
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
