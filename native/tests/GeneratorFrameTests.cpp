#include "../runtime/ByteBuffer.h"
#include "../runtime/GeneratorFrame.h"
#include "../runtime/GeneratorTasks.h"
#include "../runtime/HostArrays.h"
#include "../runtime/String.h"
#include "../runtime/StringArray.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
#define CHECK(condition)                                                                            \
  do {                                                                                             \
    if (!(condition)) {                                                                             \
      std::fprintf(stderr, "generator assertion %d: %s\n", __LINE__, #condition);                  \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
namespace {
void nothing(void *) noexcept {}
bool copyInteger(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  std::memcpy(out, in, sizeof(std::int64_t));
  return true;
}
const ValueLayout integer{0x47454e01, sizeof(std::int64_t), alignof(std::int64_t), copyInteger,
                          nothing};
struct Probe {
  unsigned constructions = 0, resumes = 0, destructions = 0;
};
struct Input {
  Probe *probe;
  std::int64_t count;
  unsigned mode = 0;
  std::atomic<bool> *cancellation = nullptr;
  std::atomic<unsigned> *barrier = nullptr;
};
struct Frame {
  Input input;
  std::int64_t position = 0;
};
bool construct(Invocation &, void *p, const Packet &packet) {
  CHECK(packet.layout == 31 && packet.bytes == sizeof(Input));
  const auto input = *static_cast<const Input *>(packet.data);
  new (p) Frame{input};
  ++input.probe->constructions;
  return true;
}
void destroy(void *p) noexcept {
  auto &frame = *static_cast<Frame *>(p);
  ++frame.input.probe->destructions;
  frame.~Frame();
}
GeneratorStep resume(Invocation &ctx, void *p, void *out) {
  auto &frame = *static_cast<Frame *>(p);
  ++frame.input.probe->resumes;
  if (frame.input.mode == 6) {
    frame.input.barrier->store(1, std::memory_order_release);
    while (frame.input.barrier->load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }
  if (frame.position == 2) {
    switch (frame.input.mode) {
    case 1:
      *static_cast<std::int64_t *>(out) = -7;
      return GeneratorStep::Error;
    case 2:
      ctx.fault(5, 13, 9);
      return GeneratorStep::Stopped;
    case 3:
      if (!ctx.allocate(65536))
        return GeneratorStep::Stopped;
      break;
    case 4:
    case 5:
      frame.input.cancellation->store(true, std::memory_order_release);
      if (frame.input.mode == 5)
        ctx.fault(5, 13, 9);
      return GeneratorStep::Stopped;
    default:
      break;
    }
  }
  if (frame.position == frame.input.count)
    return GeneratorStep::Exhausted;
  *static_cast<std::int64_t *>(out) = frame.position * frame.position;
  ++frame.position;
  return GeneratorStep::Item;
}
const GeneratorEntry entry{1, sizeof(Frame), alignof(Frame), construct, destroy, resume, &integer,
                            nullptr};
const GeneratorEntry fallible{1, sizeof(Frame), alignof(Frame), construct, destroy, resume, &integer,
                               &integer};
GeneratorDescriptor create(Invocation &ctx, Input input, bool canFail = false) {
  GeneratorDescriptor result;
  const Packet packet{31, &input, sizeof input, &ctx};
  kalandra_generator_create_v1(&ctx, &result, canFail ? &fallible : &entry, &packet);
  return result;
}
GeneratorStep next(Invocation &ctx, const GeneratorDescriptor &desc, std::int64_t &value) {
  return GeneratorStep(kalandra_generator_next_v1(&ctx, &desc, &integer, nullptr, &value, nullptr));
}
void independent() {
  Invocation ctx;
  auto *scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(scope);
  Probe first, second;
  auto a = create(ctx, {&first, 1000000000});
  auto b = create(ctx, {&second, 2});
  CHECK(first.constructions == 1 && second.constructions == 1);
  CHECK(first.resumes == 0 && second.resumes == 0 && !ctx.poll());
  const auto allocations = ctx.allocations();
  std::int64_t value = -1;
  for (std::int64_t i = 0; i < 7; ++i)
    CHECK(next(ctx, a, value) == GeneratorStep::Item && value == i * i);
  CHECK(first.resumes == 7 && second.resumes == 0 && ctx.allocations() == allocations);
  CHECK(next(ctx, b, value) == GeneratorStep::Item && value == 0);
  CHECK(next(ctx, b, value) == GeneratorStep::Item && value == 1);
  CHECK(next(ctx, b, value) == GeneratorStep::Exhausted && second.destructions == 1);
  for (unsigned i = 0; i < 5; ++i)
    CHECK(next(ctx, b, value) == GeneratorStep::Exhausted);
  CHECK(second.resumes == 3 && first.destructions == 0);
  CHECK(generatorClose(ctx, a) && first.destructions == 1);
  CHECK(generatorClose(ctx, a));
  CHECK(next(ctx, a, value) == GeneratorStep::Exhausted && first.resumes == 7);
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
  CHECK(first.destructions == 1 && second.destructions == 1);
  ctx.settle();
  CHECK(ctx.live() == 0);
}
void failures() {
  for (unsigned mode : {1u, 2u, 4u, 5u}) {
    Probe probe;
    Invocation ctx;
    std::atomic<bool> cancellation{false};
    ctx.bindCancellation(&cancellation);
    CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
    auto cursor = create(ctx, {&probe, 10, mode, &cancellation}, mode == 1);
    std::int64_t item = -1, error = 0;
    for (unsigned i = 0; i < 2; ++i)
      CHECK(generatorNext(ctx, cursor, integer, mode == 1 ? &integer : nullptr, &item, &error) ==
            GeneratorStep::Item);
    const auto result =
        generatorNext(ctx, cursor, integer, mode == 1 ? &integer : nullptr, &item, &error);
    CHECK(probe.destructions == 1 && probe.resumes == 3);
    if (mode == 1) {
      CHECK(result == GeneratorStep::Error && error == -7 && !ctx.poll());
      CHECK(generatorNext(ctx, cursor, integer, &integer, &item, &error) == GeneratorStep::Exhausted);
      CHECK(probe.resumes == 3);
    } else {
      CHECK(result == GeneratorStep::Stopped);
      if (mode == 4)
        CHECK(ctx.poll() == -1 && ctx.status() == 0);
      else
        CHECK(ctx.status() == 5 && ctx.line() == 13 && ctx.column() == 9);
    }
    ctx.settle();
    CHECK(ctx.live() == 0 && probe.destructions == 1);
  }
}
struct Budget {
  std::uint64_t bytes = 0, maximum = 16384;
  std::atomic<bool> *cancel = nullptr;
  static bool reserve(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<Budget *>(p);
    if (bytes > s.maximum - s.bytes)
      return false;
    s.bytes += bytes;
    if (s.cancel)
      s.cancel->store(true, std::memory_order_release);
    return true;
  }
  static void release(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<Budget *>(p);
    CHECK(bytes <= s.bytes);
    s.bytes -= bytes;
  }
};
struct BufferCapture {
  BufferDescriptor buffer;
  Invocation **storage;
  std::atomic<unsigned> *barrier = nullptr;
};
struct BufferFrame {
  BufferDescriptor buffer;
  std::atomic<unsigned> *barrier;
};
bool constructBuffer(Invocation &ctx, void *out, const Packet &packet) {
  const auto &capture = *static_cast<const BufferCapture *>(packet.data);
  if (!packet.source || !ctx.retainFrom(*packet.source, capture.buffer.storage.allocated))
    return false;
  auto *frame = new (out) BufferFrame{capture.buffer, capture.barrier};
  const OwnedDescriptor *owned[]{&frame->buffer};
  if (!kalandra_file_scope_enter_v1(&ctx, owned, 1))
    return false;
  *capture.storage = &ctx;
  return true;
}
void destroyBuffer(void *p) noexcept { static_cast<BufferFrame *>(p)->~BufferFrame(); }
GeneratorStep endBuffer(Invocation &ctx, void *p, void *) {
  auto &frame = *static_cast<BufferFrame *>(p);
  if (frame.barrier) {
    frame.barrier->store(1, std::memory_order_release);
    for (unsigned i = 0; i < 1000; ++i) {
      CHECK(ctx.allocate(16));
      if (i % 10 == 0)
        std::this_thread::yield();
    }
    while (frame.barrier->load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    CHECK(ctx.allocate(16));
  }
  return GeneratorStep::Exhausted;
}
void pausedResourceGrowth() {
  Budget budget;
  budget.maximum = 1024 * 1024;
  Invocation creator, worker;
  creator.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
  CHECK(kalandra_file_scope_enter_v1(&creator, nullptr, 0));
  BufferArguments arguments{};
  BufferResult created{};
  kalandra_buffer_v1(&creator, &created, unsigned(kalandra::BufferAction::Create), &arguments);
  CHECK(!creator.poll() && created.kind == 0);
  Invocation *storage = nullptr;
  std::atomic<unsigned> barrier{0};
  BufferCapture input{created.buffer, &storage, &barrier};
  const GeneratorEntry entry{1,
                             sizeof(BufferFrame),
                             alignof(BufferFrame),
                             constructBuffer,
                             destroyBuffer,
                             endBuffer,
                             &integer,
                             nullptr};
  auto cursor = generatorCreate(creator, entry, {79, &input, sizeof(input), &creator});
  CHECK(!creator.poll() && storage);
  CHECK(worker.retainFrom(*storage, input.buffer.storage.allocated));
  const OwnedDescriptor *transferred[]{&input.buffer};
  CHECK(kalandra_file_scope_enter_v1(&worker, transferred, 1));
  const auto before = budget.bytes;
  arguments.buffer = input.buffer;
  arguments.value = 1024;
  BufferResult grown{};
  kalandra_buffer_v1(&worker, &grown, unsigned(kalandra::BufferAction::Reserve), &arguments);
  CHECK(!worker.poll() && grown.kind == 0 && bufferStats(worker, input.buffer).capacity >= 1024);
  CHECK(budget.bytes > before && budget.bytes <= budget.maximum && !creator.poll());
  Invocation consumer;
  CHECK(consumer.retainFrom(creator, cursor.storage.allocated));
  GeneratorStep result = GeneratorStep::Stopped;
  std::thread advancing([&] {
    std::int64_t ignored = 0;
    result = generatorNext(consumer, cursor, integer, nullptr, &ignored, nullptr);
  });
  while (!barrier.load(std::memory_order_acquire))
    std::this_thread::yield();
  arguments.value = 8192;
  kalandra_buffer_v1(&worker, &grown, unsigned(kalandra::BufferAction::Reserve), &arguments);
  CHECK(!worker.poll() && grown.kind == 0 && bufferStats(worker, input.buffer).capacity >= 8192);
  barrier.store(2, std::memory_order_release);
  advancing.join();
  CHECK(result == GeneratorStep::Exhausted && !consumer.poll() && !creator.poll());
  worker.settle();
  CHECK(generatorClose(creator, cursor));
  creator.settle();
  consumer.settle();
  CHECK(budget.bytes == 0 && creator.live() == 0 && worker.live() == 0 && consumer.live() == 0);
}
void destroyBudgetAnchor(void *p) noexcept { static_cast<OwnedState *>(p)->~OwnedState(); }
void ignoreBudgetAnchor(void *) noexcept {}
void deepResourceBudgets() {
  constexpr unsigned depth = 10000;
  Invocation owner;
  std::vector<OwnedState *> anchors;
  for (unsigned i = 0; i <= depth; ++i) {
    auto *memory = owner.allocateResource(sizeof(OwnedState), OwnedState::cleanup, true);
    CHECK(memory);
    anchors.push_back(new (memory) OwnedState(OwnedKind::Generator, sizeof(OwnedState),
                                              destroyBudgetAnchor, ignoreBudgetAnchor));
    CHECK(owner.trackResource(memory));
  }
  std::vector<std::unique_ptr<Invocation>> contexts(depth);
  // Build bottom-up: each anchor has no ancestors until its child is ready.
  // This isolates one deep growth operation instead of quadratic construction.
  for (unsigned i = depth; i > 0; --i) {
    auto &context = contexts[i - 1];
    context = std::make_unique<Invocation>();
    context->bindResourceBudget(owner, anchors[i - 1]);
    CHECK(!context->poll() && context->retainFrom(owner, anchors[i]));
  }
  Budget budget;
  budget.maximum = 64 * 1024 * 1024;
  Invocation root;
  root.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
  CHECK(root.retainFrom(owner, anchors.front()));
  const auto before = budget.bytes;
  CHECK(owner.resizeResource(anchors.back(), 65536));
  CHECK(budget.bytes == before + 65536);
  // A failed owner reservation must leave the entire deep graph unchanged.
  budget.maximum = budget.bytes;
  CHECK(!owner.resizeResource(anchors.back(), 131072) && owner.status() == 2);
  CHECK(budget.bytes == before + 65536);
  for (auto &context : contexts)
    context.reset();
  root.settle();
  owner.settle();
  CHECK(budget.bytes == 0 && root.live() == 0 && owner.live() == 0);
}
void sharedResourceBudgets() {
  Invocation owner;
  std::array<OwnedState *, 4> anchors{};
  for (auto &anchor : anchors) {
    auto *memory = owner.allocateResource(sizeof(OwnedState), OwnedState::cleanup, true);
    CHECK(memory);
    anchor = new (memory) OwnedState(OwnedKind::Generator, sizeof(OwnedState), destroyBudgetAnchor,
                                     ignoreBudgetAnchor);
    CHECK(owner.trackResource(memory));
  }
  Invocation a, b, grand;
  a.bindResourceBudget(owner, anchors[1]);
  b.bindResourceBudget(owner, anchors[2]);
  CHECK(a.retainFrom(owner, anchors[3]) && b.retainFrom(owner, anchors[3]));
  grand.bindResourceBudget(owner, anchors[0]);
  CHECK(grand.retainFrom(owner, anchors[1]) && grand.retainFrom(owner, anchors[2]));
  Budget first, second;
  first.maximum = second.maximum = 1024 * 1024;
  Invocation root, direct, rejected, accepted, cancelled;
  root.bindRecursiveBudget(&first, Budget::reserve, Budget::release);
  direct.bindRecursiveBudget(&second, Budget::reserve, Budget::release);
  CHECK(root.retainFrom(owner, anchors[0]) && direct.retainFrom(owner, anchors[3]));
  CHECK(rejected.retainFrom(owner, anchors[3]) && accepted.retainFrom(owner, anchors[3]) &&
        cancelled.retainFrom(owner, anchors[3]));
  const auto beforeFirst = first.bytes, beforeSecond = second.bytes;
  first.maximum = beforeFirst + 8191;
  CHECK(!rejected.resizeResource(anchors[3], 4096) && rejected.status() == 2);
  CHECK(first.bytes == beforeFirst && second.bytes == beforeSecond);
  first.maximum = 1024 * 1024;
  CHECK(accepted.resizeResource(anchors[3], 4096));
  // Each independent retained owner reserves its path, without recursive
  // propagation or losing either branch of the shared ancestor's transaction.
  CHECK(first.bytes == beforeFirst + 8192 && second.bytes == beforeSecond + 4096);
  const auto heldFirst = first.bytes, heldSecond = second.bytes;
  std::atomic<bool> token{false};
  cancelled.bindCancellation(&token);
  first.cancel = &token;
  CHECK(!cancelled.resizeResource(anchors[3], 8192) && cancelled.status() == 0 &&
        cancelled.poll() == -1);
  CHECK(first.bytes == heldFirst && second.bytes == heldSecond);
  first.cancel = nullptr;
  grand.settle();
  a.settle();
  b.settle();
  root.settle();
  direct.settle();
  rejected.settle();
  accepted.settle();
  cancelled.settle();
  owner.settle();
  CHECK(first.bytes == 0 && second.bytes == 0);
}
void multipleFrameLedgers() {
  Budget budget;
  budget.maximum = 8 * 1024 * 1024;
  Invocation owner, body, outcome;
  owner.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
  auto *memory = owner.allocateResource(sizeof(OwnedState), OwnedState::cleanup, true);
  CHECK(memory);
  auto *anchor = new (memory)
      OwnedState(OwnedKind::Generator, sizeof(OwnedState), destroyBudgetAnchor, ignoreBudgetAnchor);
  CHECK(owner.trackResource(anchor));
  body.bindResourceBudget(owner, anchor);
  CHECK(body.allocate(4096) && !body.poll());
  // A fresh outcome ledger can bind after the body already has reservations.
  outcome.bindResourceBudget(owner, anchor);
  CHECK(outcome.allocate(8192) && !outcome.poll());
  auto peak = budget.bytes;
  body.settle();
  CHECK(budget.bytes == peak); // ownership scope keeps its existing high-water charge
  Invocation replacement;
  replacement.bindResourceBudget(owner, anchor);
  CHECK(replacement.allocate(4096) && !replacement.poll() && budget.bytes == peak);
  replacement.settle();
  Invocation workerA, workerB;
  workerA.bindResourceBudget(owner, anchor);
  workerB.bindResourceBudget(owner, anchor);
  const auto allocate = [](Invocation &ctx) {
    for (unsigned i = 0; i < 1000; ++i)
      CHECK(ctx.allocate(64));
  };
  std::thread a([&] { allocate(workerA); });
  std::thread b([&] { allocate(workerB); });
  a.join();
  b.join();
  CHECK(!workerA.poll() && !workerB.poll() && budget.bytes <= budget.maximum);
  peak = budget.bytes;
  // Rebinding an existing ledger remains invalid and changes no reservations.
  workerA.bindResourceBudget(owner, anchor);
  CHECK(workerA.status() == 3 && budget.bytes == peak);
  workerA.settle();
  workerB.settle();
  CHECK(budget.bytes == peak);
  outcome.settle();
  owner.settle();
  CHECK(budget.bytes == 0 && owner.live() == 0 && outcome.live() == 0);
}
void bounded() {
  Budget budget;
  Probe probe;
  {
    Invocation ctx;
    ctx.bindRecursiveBudget(&budget, Budget::reserve, Budget::release);
    CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
    auto cursor = create(ctx, {&probe, 10, 3});
    CHECK(!ctx.poll());
    std::int64_t value;
    CHECK(next(ctx, cursor, value) == GeneratorStep::Item);
    CHECK(next(ctx, cursor, value) == GeneratorStep::Item);
    CHECK(next(ctx, cursor, value) == GeneratorStep::Stopped && ctx.status() == 2);
    CHECK(probe.destructions == 1 && budget.bytes <= budget.maximum);
  }
  CHECK(budget.bytes == 0);
}
void ownerBudgets() {
  Budget first, second;
  second.maximum = 131072;
  Probe probe;
  {
    Invocation creator, borrower;
    creator.bindRecursiveBudget(&first, Budget::reserve, Budget::release);
    borrower.bindRecursiveBudget(&second, Budget::reserve, Budget::release);
    CHECK(kalandra_file_scope_enter_v1(&creator, nullptr, 0));
    auto cursor = create(creator, {&probe, 10, 3});
    CHECK(borrower.retainFrom(creator, cursor.storage.allocated));
    const auto before = second.bytes;
    std::int64_t value;
    CHECK(next(borrower, cursor, value) == GeneratorStep::Item);
    CHECK(next(borrower, cursor, value) == GeneratorStep::Item);
    CHECK(next(borrower, cursor, value) == GeneratorStep::Stopped && borrower.status() == 2);
    // A permissive caller cannot bypass the smaller surviving owner's budget;
    // rejected reservations must roll back the other owner's pending charge.
    CHECK(second.bytes == before && first.bytes <= first.maximum && !creator.poll());
  }
  CHECK(first.bytes == 0 && second.bytes == 0 && probe.destructions == 1);
}
void activeClose() {
  Probe probe;
  std::atomic<unsigned> barrier{0};
  Invocation creator, borrower;
  CHECK(kalandra_file_scope_enter_v1(&creator, nullptr, 0));
  auto cursor = create(creator, {&probe, 2, 6, nullptr, &barrier});
  CHECK(borrower.retainFrom(creator, cursor.storage.allocated));
  GeneratorStep result = GeneratorStep::Item;
  std::thread thread([&] {
    std::int64_t value;
    result = next(borrower, cursor, value);
  });
  while (barrier.load(std::memory_order_acquire) != 1)
    std::this_thread::yield();
  // A source program cannot acquire this second mutable capability. The
  // private ABI still rejects overlap without blocking or touching live locals.
  CHECK(!generatorClose(creator, cursor) && creator.status() == 3);
  creator.settle();
  barrier.store(2, std::memory_order_release);
  thread.join();
  CHECK(result == GeneratorStep::Stopped && probe.destructions == 1 && borrower.status() == 3);
  borrower.settle();
}
bool copyString(Invocation &to, void *out, const Invocation &from, const void *in) noexcept {
  auto &value = *static_cast<const StringDescriptor *>(in);
  if (!kalandra_string_retain_v1(&to, &from, &value))
    return false;
  *static_cast<StringDescriptor *>(out) = value;
  return true;
}
const ValueLayout string{0x47454e02, sizeof(StringDescriptor), alignof(StringDescriptor), copyString,
                         nothing};
bool textConstruct(Invocation &ctx, void *out, const Packet &packet) {
  return packet.source && copyString(ctx, out, *packet.source, packet.data);
}
GeneratorStep textResume(Invocation &, void *in, void *out) {
  *static_cast<StringDescriptor *>(out) = *static_cast<StringDescriptor *>(in);
  return GeneratorStep::Item;
}
const GeneratorEntry textEntry{1, sizeof(StringDescriptor), alignof(StringDescriptor), textConstruct,
                                nothing, textResume, &string, nullptr};
GeneratorStep textError(Invocation &ctx, void *in, void *out) {
  (void)textResume(ctx, in, out);
  return GeneratorStep::Error;
}
const GeneratorEntry textErrorEntry{1, sizeof(StringDescriptor), alignof(StringDescriptor),
                                     textConstruct, nothing, textError, &integer, &string};
void typedTextFailure() {
  Invocation ctx;
  CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  StringDescriptor input;
  kalandra_string_literal_v1(&ctx, &input, "bad input", 9);
  auto cursor = generatorCreate(ctx, textErrorEntry, {32, &input, sizeof input, &ctx});
  std::int64_t item = -1;
  StringDescriptor failure;
  CHECK(generatorNext(ctx, cursor, integer, &string, &item, &failure) == GeneratorStep::Error);
  CHECK(validateString(ctx, failure) && std::string_view(failure.data, failure.byteLength) == "bad input");
  CHECK(generatorNext(ctx, cursor, integer, &string, &item, &failure) == GeneratorStep::Exhausted);
  ctx.settle();
  CHECK(ctx.live() == 0);
}
void outlivesCreator() {
  Budget receiverBudget;
  receiverBudget.maximum = 262144;
  Invocation receiver;
  receiver.bindRecursiveBudget(&receiverBudget, Budget::reserve, Budget::release);
  auto *scope = kalandra_file_scope_enter_v1(&receiver, nullptr, 0);
  CHECK(scope);
  GeneratorDescriptor textCursor, scalarCursor;
  Probe probe;
  {
    // Neither this context nor its cancellation token survives construction.
    Budget creatorBudget;
    Invocation creator;
    creator.bindRecursiveBudget(&creatorBudget, Budget::reserve, Budget::release);
    std::atomic<bool> token{false};
    creator.bindCancellation(&token);
    auto *sourceScope = kalandra_file_scope_enter_v1(&creator, nullptr, 0);
    CHECK(sourceScope);
    StringDescriptor input;
    kalandra_string_literal_v1(&creator, &input, "hello", 5);
    textCursor = generatorCreate(creator, textEntry, {32, &input, sizeof input, &creator});
    scalarCursor = create(creator, {&probe, 3, 3});
    CHECK(!creator.poll());
    CHECK(receiver.retainFrom(creator, textCursor.storage.allocated));
    CHECK(receiver.retainFrom(creator, scalarCursor.storage.allocated));
    const OwnedDescriptor *inputs[]{&textCursor, &scalarCursor};
    auto *transfer = kalandra_file_scope_enter_v1(&receiver, inputs, 2);
    CHECK(transfer);
    // Move both frames to the receiver's surviving parent scope.
    adopt(receiver, *inspectOwned(receiver, textCursor), static_cast<ResourceControl *>(scope)->scope);
    adopt(receiver, *inspectOwned(receiver, scalarCursor), static_cast<ResourceControl *>(scope)->scope);
    kalandra_file_scope_leave_v1(&receiver, transfer, nullptr);
  }
  StringDescriptor result;
  CHECK(generatorNext(receiver, textCursor, string, nullptr, &result, nullptr) == GeneratorStep::Item);
  CHECK(generatorClose(receiver, textCursor));
  CHECK(validateString(receiver, result) && std::string_view(result.data, result.byteLength) == "hello");
  std::int64_t value = -1;
  CHECK(next(receiver, scalarCursor, value) == GeneratorStep::Item && value == 0);
  CHECK(next(receiver, scalarCursor, value) == GeneratorStep::Item && value == 1);
  CHECK(next(receiver, scalarCursor, value) == GeneratorStep::Item && value == 4);
  CHECK(receiverBudget.bytes >= 65536 && receiverBudget.bytes <= receiverBudget.maximum);
  CHECK(generatorClose(receiver, scalarCursor));
  CHECK(probe.destructions == 1);
  kalandra_file_scope_leave_v1(&receiver, scope, nullptr);
  receiver.settle();
  CHECK(receiver.live() == 0 && receiverBudget.bytes == 0);
}
void invalid() {
  for (unsigned attempt = 1; attempt <= 12; ++attempt) {
    Probe probe;
    Invocation ctx;
    ctx.failAllocationAt(attempt);
    auto scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
    if (scope) {
      (void)create(ctx, {&probe, 5});
      kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
    }
    ctx.settle();
    CHECK(probe.destructions == probe.constructions && ctx.live() == 0);
  }
  Probe probe;
  Invocation ctx;
  auto cursor = create(ctx, {&probe, 2});
  ValueLayout other = integer;
  ++other.identity;
  std::int64_t value;
  CHECK(generatorNext(ctx, cursor, other, nullptr, &value, nullptr) == GeneratorStep::Stopped);
  CHECK(ctx.status() == 3 && probe.resumes == 0);
  ctx.settle();
  CHECK(probe.destructions == 1);
}
void rangeSources() {
  struct Case {
    IntegerRangeValue input;
    std::vector<std::int64_t> expected;
  };
  for (const auto &test : {Case{{0, 7, 2}, {0, 2, 4, 6}}, Case{{5, -1, -2}, {5, 3, 1}},
                           Case{{0, 0, 1}, {}}, Case{{0, 10, -1}, {}}, Case{{10, 0, 1}, {}},
                           Case{{INT64_MAX - 2, INT64_MAX, 3}, {INT64_MAX - 2}},
                           Case{{INT64_MIN + 2, INT64_MIN, -3}, {INT64_MIN + 2}},
                           Case{{INT64_MAX, INT64_MIN, INT64_MIN}, {INT64_MAX, -1}}}) {
    Invocation ctx;
    CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
    GeneratorDescriptor cursor;
    kalandra_iterator_range_v1(&ctx, &cursor, &test.input, &integer, 23, 5);
    CHECK(!ctx.poll());
    // The factory's stack-local entry and packet are already gone here.
    for (auto expected : test.expected) {
      std::int64_t value = 0;
      CHECK(next(ctx, cursor, value) == GeneratorStep::Item && value == expected);
    }
    std::int64_t value = 7;
    CHECK(next(ctx, cursor, value) == GeneratorStep::Exhausted && value == 7);
    CHECK(next(ctx, cursor, value) == GeneratorStep::Exhausted);
    ctx.settle();
    CHECK(ctx.live() == 0);
  }
  Invocation ctx;
  CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  const IntegerRangeValue input{10, INT64_MAX, 1};
  auto first = generatorRange(ctx, input, integer, 1, 1);
  auto second = generatorRange(ctx, input, integer, 1, 1);
  auto allocations = ctx.allocations();
  std::int64_t value;
  CHECK(next(ctx, first, value) == GeneratorStep::Item && value == 10);
  CHECK(next(ctx, first, value) == GeneratorStep::Item && value == 11);
  CHECK(next(ctx, second, value) == GeneratorStep::Item && value == 10 && input.start == 10);
  CHECK(ctx.allocations() == allocations);
  CHECK(generatorClose(ctx, first) && generatorClose(ctx, second));
  ctx.settle();
  CHECK(ctx.live() == 0);
  Invocation zero;
  (void)generatorRange(zero, {0, 0, 0}, integer, 23, 5);
  CHECK(zero.status() == 4 && zero.line() == 23 && zero.column() == 5 && zero.allocations() == 0);
  Invocation cancelled;
  std::atomic<bool> token{true};
  cancelled.bindCancellation(&token);
  (void)generatorRange(cancelled, {0, 0, 0}, integer, 23, 5);
  CHECK(cancelled.status() == 0 && cancelled.poll() == -1);
}
template <kalandra::NumericType Element> void numericTensorPublication() {
  Invocation source, receiver;
  using Scalar = NumericValue<Element>;
  auto *values = static_cast<Scalar *>(source.allocate(16 * sizeof(Scalar)));
  CHECK(values);
  for (unsigned index = 0; index < 16; ++index)
    values[index] = Scalar(index * 2 + 3);
  CpuTensor<Element, 2> view{values, values + 1, 1, {2, 2}, {4, 2}}, packed{};
  CHECK(kalandra_record_tensor_pack_v1(&receiver, &packed, &source, &view, 2, unsigned(Element)));
  CHECK(packed.allocated == packed.aligned && packed.allocated != values && !packed.offset);
  CHECK((packed.sizes == std::array<std::int64_t, 2>{2, 2}));
  CHECK((packed.strides == std::array<std::int64_t, 2>{2, 1}));
  for (unsigned index = 0; index < 4; ++index)
    CHECK(packed.aligned[index] == Scalar(index * 4 + 7));
  CpuTensor<Element, 2> alias{};
  CHECK(
      kalandra_record_tensor_pack_v1(&receiver, &alias, &receiver, &packed, 2, unsigned(Element)));
  CHECK(alias.allocated == packed.allocated); // Already packed storage is retained.
  CpuTensor<Element, 0> scalar{values, values + 1, 1}, scalarPacked{};
  CHECK(kalandra_record_tensor_pack_v1(&receiver, &scalarPacked, &source, &scalar, 0,
                                       unsigned(Element)));
  CHECK(scalarPacked.aligned[0] == Scalar(7) && !scalarPacked.offset);
  CpuTensor<Element, 2> empty{values, values, 1, {0, INT64_MAX}, {INT64_MAX, 1}}, emptyPacked{};
  CHECK(kalandra_record_tensor_pack_v1(&receiver, &emptyPacked, &source, &empty, 2,
                                       unsigned(Element)));
  CHECK(!emptyPacked.allocated && !emptyPacked.aligned && !emptyPacked.offset);
  CHECK((emptyPacked.strides == std::array<std::int64_t, 2>{0, 0}));
  source.settle();
  CHECK(packed.aligned[3] == Scalar(19) && scalarPacked.aligned[0] == Scalar(7));
  receiver.settle();
  CHECK(!source.live() && !receiver.live());
}
template <kalandra::NumericType Element> void numericArraySources() {
  numericTensorPublication<Element>();
  Invocation ctx;
  using Scalar = NumericValue<Element>;
  auto *values = static_cast<Scalar *>(ctx.allocate(5 * sizeof(Scalar)));
  CHECK(values);
  for (unsigned n = 0; n < 5; ++n)
    values[n] = Scalar(2 * n + 3);
  CpuTensor<Element, 1> source{values, values, 1, {3}, {1}};
  const auto &item = hostScalarLayout<Element>();
  auto left = generatorArray(ctx, hostArrayLayout<Element>(), item, &source);
  auto right = generatorArray(ctx, hostArrayLayout<Element>(), item, &source);
  CHECK(!ctx.poll());
  source.sizes[0] = 0; // Cursor metadata is a snapshot, not a descriptor borrow.
  const auto allocations = ctx.allocations();
  Scalar value = 0;
  for (unsigned n = 0; n < 3; ++n)
    CHECK(generatorNext(ctx, left, item, nullptr, &value, nullptr) == GeneratorStep::Item &&
          value == Scalar(2 * n + 5));
  CHECK(generatorNext(ctx, left, item, nullptr, &value, nullptr) == GeneratorStep::Exhausted);
  CHECK(generatorNext(ctx, left, item, nullptr, &value, nullptr) == GeneratorStep::Exhausted);
  CHECK(generatorNext(ctx, right, item, nullptr, &value, nullptr) == GeneratorStep::Item &&
        value == Scalar(5));
  CHECK(ctx.allocations() == allocations);
  CHECK(generatorClose(ctx, left) && generatorClose(ctx, right));
  CpuTensor<Element, 1> empty{nullptr, nullptr, 0, {0}, {1}};
  auto absent = generatorArray(ctx, hostArrayLayout<Element>(), item, &empty);
  CHECK(!ctx.poll());
  CHECK(generatorNext(ctx, absent, item, nullptr, &value, nullptr) == GeneratorStep::Exhausted);
  CHECK(generatorClose(ctx, absent));
  empty.strides[0] = 0; // Compiler-generated empty numeric arrays use a zero stride.
  absent = generatorArray(ctx, hostArrayLayout<Element>(), item, &empty);
  CHECK(!ctx.poll());
  CHECK(generatorNext(ctx, absent, item, nullptr, &value, nullptr) == GeneratorStep::Exhausted);
  CHECK(generatorClose(ctx, absent));
  ctx.settle();
  CHECK(ctx.live() == 0);
}
bool copyStringArray(Invocation &to, void *out, const Invocation &from, const void *in) noexcept {
  if (!kalandra_string_array_retain_v1(&to, &from, static_cast<const StringArrayDescriptor *>(in)))
    return false;
  std::memcpy(out, in, sizeof(StringArrayDescriptor));
  return true;
}
const ValueLayout stringArray{0x47454e03, sizeof(StringArrayDescriptor),
                              alignof(StringArrayDescriptor), copyStringArray, nothing};
void arrayBackingLifetime() {
  Invocation owner;
  GeneratorDescriptor cursor;
  {
    Invocation creator;
    auto *values = static_cast<StringDescriptor *>(creator.allocate(2 * sizeof(StringDescriptor)));
    CHECK(values);
    kalandra_string_literal_v1(&creator, &values[0], "alpha", 5);
    kalandra_string_literal_v1(&creator, &values[1], "beta", 4);
    StringArrayDescriptor source{values, values, 0, 2, 1};
    cursor = generatorArray(creator, stringArray, string, &source);
    CHECK(!creator.poll());
    CHECK(owner.retainFrom(creator, cursor.storage.allocated));
  }
  StringDescriptor first, second;
  CHECK(generatorNext(owner, cursor, string, nullptr, &first, nullptr) == GeneratorStep::Item);
  CHECK(generatorNext(owner, cursor, string, nullptr, &second, nullptr) == GeneratorStep::Item);
  CHECK(generatorClose(owner, cursor));
  CHECK(validateString(owner, first) && std::string_view(first.data, first.byteLength) == "alpha");
  CHECK(validateString(owner, second) &&
        std::string_view(second.data, second.byteLength) == "beta");
  owner.settle();
  CHECK(owner.live() == 0);
}
void arraySourceFailures() {
  for (unsigned attempt = 1; attempt <= 16; ++attempt) {
    Invocation source, target;
    auto *values = static_cast<std::int64_t *>(source.allocate(16));
    CHECK(values);
    values[0] = 7;
    values[1] = 9;
    CpuTensor<kalandra::NumericType::Int64, 1> descriptor{values, values, 0, {2}, {1}};
    target.failAllocationAt(attempt);
    // A failed item retain or frame allocation must not strand a source lease.
    CHECK(target.retainFrom(source, values) || target.status() == 2);
    if (!target.poll()) {
      const auto &layout = hostArrayLayout<kalandra::NumericType::Int64>();
      auto cursor = generatorArray(target, layout, integer, &descriptor);
      if (!target.poll())
        CHECK(generatorClose(target, cursor));
    }
    target.settle();
    source.settle();
    CHECK(target.live() == 0 && source.live() == 0);
  }
  for (unsigned mode = 0; mode < 5; ++mode) {
    Invocation ctx;
    auto *values = static_cast<std::int64_t *>(ctx.allocate(16));
    CHECK(values);
    CpuTensor<kalandra::NumericType::Int64, 1> descriptor{values, values, 0, {2}, {1}};
    if (mode == 0)
      descriptor.offset = -1;
    else if (mode == 1)
      descriptor.sizes[0] = -1;
    else if (mode == 2)
      descriptor.strides[0] = 0;
    else if (mode == 3)
      descriptor.sizes[0] = 3;
    else
      descriptor.offset = INT64_MAX;
    (void)generatorArray(ctx, hostArrayLayout<kalandra::NumericType::Int64>(), integer,
                         &descriptor);
    CHECK(ctx.status() == 3);
    ctx.settle();
    CHECK(ctx.live() == 0);
  }
  Invocation cancelled;
  std::atomic<bool> token{true};
  cancelled.bindCancellation(&token);
  (void)generatorArray(cancelled, integer, integer, nullptr);
  CHECK(cancelled.status() == 0 && cancelled.poll() == -1 && cancelled.allocations() == 0);
}
void booleanArraySource() {
  Invocation ctx;
  auto *values = static_cast<std::uint8_t *>(ctx.allocate(3));
  CHECK(values);
  values[0] = 1;
  values[1] = 0;
  values[2] = 1;
  CpuTensor<kalandra::NumericType::UInt8, 1> descriptor{values, values, 0, {3}, {1}};
  const auto &item = hostScalarLayout<kalandra::NumericType::UInt8, true>();
  auto cursor =
      generatorArray(ctx, hostArrayLayout<kalandra::NumericType::UInt8, true>(), item, &descriptor);
  for (auto expected : {1u, 0u, 1u}) {
    std::uint8_t result = 2;
    CHECK(generatorNext(ctx, cursor, item, nullptr, &result, nullptr) == GeneratorStep::Item &&
          result == expected);
  }
  CHECK(generatorClose(ctx, cursor));
  ctx.settle();
  CHECK(ctx.live() == 0);
}
struct RetiringItem {
  Probe *probe;
  void *scope;
  std::atomic<bool> *cancellation = nullptr;
  Invocation::OwnershipScope *releaseOwner = nullptr;
};
bool retiringConstruct(Invocation &, void *out, const Packet &packet) {
  const auto value = *static_cast<const RetiringItem *>(packet.data);
  new (out) RetiringItem(value);
  ++value.probe->constructions;
  return true;
}
void retiringDestroy(void *frame) noexcept {
  ++static_cast<RetiringItem *>(frame)->probe->destructions;
}
GeneratorStep retiringResume(Invocation &, void *frame, void *out) {
  *static_cast<RetiringItem *>(out) = *static_cast<RetiringItem *>(frame);
  return GeneratorStep::Item;
}
bool retiringCopy(Invocation &ctx, void *out, const Invocation &, const void *in) noexcept {
  const auto value = *static_cast<const RetiringItem *>(in);
  *static_cast<RetiringItem *>(out) = value;
  // Deterministically retire the owner after resume's publication precheck,
  // while the cursor is still exclusively advancing. Cleanup cannot run there.
  if (value.cancellation)
    value.cancellation->store(true, std::memory_order_release);
  else
    kalandra_file_scope_leave_v1(&ctx, value.scope, nullptr);
  if (value.releaseOwner)
    value.releaseOwner->reset();
  return true;
}
void retirementDuringPublication() {
  static const ValueLayout item{0x47454e04, sizeof(RetiringItem), alignof(RetiringItem),
                                retiringCopy, nothing};
  static const GeneratorEntry entry{1,
                                    sizeof(RetiringItem),
                                    alignof(RetiringItem),
                                    retiringConstruct,
                                    retiringDestroy,
                                    retiringResume,
                                    &item,
                                    nullptr};
  for (unsigned mode : {0u, 1u, 2u}) {
    Invocation ctx;
    Invocation::OwnershipScope backing(ctx);
    std::atomic<bool> token{false};
    ctx.bindCancellation(&token);
    Probe probe;
    RetiringItem input{&probe, nullptr, mode == 1 ? &token : nullptr,
                       mode == 2 ? &backing : nullptr},
        output{};
    GeneratorDescriptor cursor;
    {
      Invocation::OwnershipSelection selected(backing);
      input.scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
      CHECK(input.scope);
      cursor = generatorCreate(ctx, entry, {0, &input, sizeof(input), &ctx});
    }
    CHECK(generatorNext(ctx, cursor, item, nullptr, &output, nullptr) == GeneratorStep::Stopped);
    CHECK(probe.constructions == 1 && probe.destructions == 1);
    CHECK(mode == 1 ? ctx.status() == 0 && ctx.poll() == -1 : ctx.status() == 3);
    backing.reset();
    ctx.settle();
    CHECK(ctx.live() == 0);
  }
}
struct CoordinatingProbe {
  std::atomic<unsigned> constructed{0}, resumed{0}, destroyed{0}, childDestroyed{0};
  std::atomic<bool> first{false}, advance{false}, externalReady{false};
  ExternalCompletion completion;
  unsigned mode = 0;
  bool captureBuffer = false;
  Invocation *captureSource = nullptr;
  GeneratorDescriptor captured;
  TaskCore *cancelCore = nullptr;
  GroupHandle cancelGroup;
  std::atomic<unsigned> copies{0};
  std::atomic<unsigned> items{0};
  std::atomic<bool> terminalPublished{false};
};
struct CoordinatingValue {
  GeneratorStep step = GeneratorStep::Exhausted;
  StringDescriptor text;
  CoordinatingProbe *probe = nullptr;
};
bool copyCoordinatingValue(Invocation &ctx, void *out, const Invocation &source,
                          const void *input) noexcept {
  const auto &value = *static_cast<const CoordinatingValue *>(input);
  if (value.probe && value.probe->mode == 8 && ++value.probe->copies == 1)
    CHECK(value.probe->cancelCore->cancel(value.probe->cancelGroup));
  if (value.step == GeneratorStep::Item || value.step == GeneratorStep::Error) {
    if (!kalandra_string_retain_v1(&ctx, &source, &value.text))
      return false;
  } else if (value.step != GeneratorStep::Exhausted) {
    ctx.fault(3);
    return false;
  }
  new (out) CoordinatingValue(value);
  return true;
}
const ValueLayout coordinatingValue{0x47454e10, sizeof(CoordinatingValue), alignof(CoordinatingValue),
                                   copyCoordinatingValue, nothing};
GeneratorStep classifyCoordinatingValue(const void *p) noexcept {
  return static_cast<const CoordinatingValue *>(p)->step;
}
void exhaustedCoordinatingValue(void *p) noexcept { new (p) CoordinatingValue; }
bool constructCoordinatingChild(Invocation &, void *out, const Packet &packet) {
  new (out) CoordinatingProbe *(*static_cast<CoordinatingProbe *const *>(packet.data));
  return true;
}
void destroyCoordinatingChild(void *p) noexcept {
  ++(*static_cast<CoordinatingProbe **>(p))->childDestroyed;
}
void resumeCoordinatingChild(Task &task) {
  auto &probe = **static_cast<CoordinatingProbe **>(task.frame());
  if (probe.mode == 11)
    task.context().fault(9, 44, 2);
  probe.completion = task.suspendExternal();
  CHECK(probe.completion);
  probe.externalReady.store(true, std::memory_order_release);
}
const TaskEntry coordinatingChild{1, 0, sizeof(CoordinatingProbe *), alignof(CoordinatingProbe *),
                                 sizeof(CoordinatingProbe *), alignof(CoordinatingProbe *),
                                 constructCoordinatingChild, destroyCoordinatingChild,
                                 resumeCoordinatingChild, &integer};
struct CoordinatingFrame {
  CoordinatingProbe *probe;
  unsigned pc = 0;
  Invocation::OwnershipScope itemScope;
  CoordinatingFrame(Invocation &ctx, CoordinatingProbe *probe) : probe(probe), itemScope(ctx) {}
};
bool constructCoordinatingBody(Invocation &ctx, void *out, const Packet &packet) {
  auto *probe = *static_cast<CoordinatingProbe *const *>(packet.data);
  ++probe->constructed;
  if (probe->captureSource) {
    if (probe->mode == 7)
      ctx.failAllocationAt(InvocationABI::allocationAttempts(ctx) + 1);
    if (!kalandra_owned_copy_v1(&ctx, probe->captureSource, &probe->captured)) {
      auto *owned = inspectOwned(*probe->captureSource, probe->captured);
      CHECK(owned && owned->scope && owned->scope->context == probe->captureSource);
      return false;
    }
    auto *owned = inspectOwned(ctx, probe->captured);
    CHECK(owned && owned->scope && owned->scope->context == &ctx);
  }
  if (probe->mode >= 5 && probe->mode <= 7)
    return false;
  new (out) CoordinatingFrame(ctx, probe);
  return true;
}
void destroyCoordinatingBody(void *p) noexcept {
  auto &f = *static_cast<CoordinatingFrame *>(p);
  CHECK(f.probe->childDestroyed == unsigned(f.probe->resumed != 0));
  ++f.probe->destroyed;
  f.~CoordinatingFrame();
}
void resumeCoordinatingBody(Task &task) {
  auto &f = *static_cast<CoordinatingFrame *>(task.frame());
  auto &probe = *f.probe;
  ++probe.resumed;
  if (probe.captureBuffer)
    CHECK(bufferStats(task.context(), probe.captured).length == 0);
  f.itemScope.reset();
  Invocation::OwnershipSelection selected(f.itemScope);
  CoordinatingValue result;
  result.probe = f.probe;
  if (!f.pc++) {
    auto group = task.openGroup();
    CHECK(group);
    auto *input = f.probe;
    CHECK(task.spawn(group, coordinatingChild, {0, &input, sizeof(input)}));
    result.step = GeneratorStep::Item;
    kalandra_string_literal_v1(&task.context(), &result.text, "first", 5);
  } else if (probe.mode == 10 && f.pc <= 10000) {
    result.step = GeneratorStep::Item;
    kalandra_string_literal_v1(&task.context(), &result.text, "first", 5);
  } else if (probe.mode == 4) {
    task.fault(8, 37, 9);
    return;
  } else if (probe.mode == 1) {
    result.step = GeneratorStep::Error;
    kalandra_string_literal_v1(&task.context(), &result.text, "error", 5);
  }
  task.complete(&result);
}
const TaskEntry coordinatingBody{1, 0, sizeof(CoordinatingProbe *), alignof(CoordinatingProbe *),
                                sizeof(CoordinatingFrame), alignof(CoordinatingFrame),
                                constructCoordinatingBody, destroyCoordinatingBody,
                                resumeCoordinatingBody, &coordinatingValue};
const GeneratorTaskEntry coordinatingGenerator{1, &coordinatingBody, classifyCoordinatingValue,
                                               exhaustedCoordinatingValue};
struct CoordinatingConsumer {
  CoordinatingProbe *probe;
  GeneratorDescriptor cursor;
  CoordinatingValue value;
  unsigned pc = 0;
  Invocation::OwnershipScope itemScope;
  CoordinatingConsumer(Invocation &ctx, CoordinatingProbe *probe) : probe(probe), itemScope(ctx) {}
};
bool constructCoordinatingConsumer(Invocation &ctx, void *out, const Packet &packet) {
  new (out) CoordinatingConsumer(ctx, *static_cast<CoordinatingProbe *const *>(packet.data));
  return true;
}
void destroyCoordinatingConsumer(void *p) noexcept {
  static_cast<CoordinatingConsumer *>(p)->~CoordinatingConsumer();
}
void resumeCoordinatingConsumer(Task &task) {
  auto &f = *static_cast<CoordinatingConsumer *>(task.frame());
  auto &probe = *f.probe;
  if (f.pc == 0) {
    void *captureScope = nullptr;
    if (probe.captureBuffer) {
      captureScope = kalandra_file_scope_enter_v1(&task.context(), nullptr, 0);
      CHECK(captureScope);
      BufferArguments arguments{};
      BufferResult result{};
      kalandra_buffer_v1(&task.context(), &result, unsigned(kalandra::BufferAction::Create),
                         &arguments);
      CHECK(!task.context().poll() && result.kind == 0);
      probe.captureSource = &task.context();
      probe.captured = result.buffer;
    }
    auto *input = f.probe;
    f.cursor = generatorTaskCreate(task, coordinatingGenerator,
                                   {0, &input, sizeof(input), &task.context()});
    if (task.context().poll())
      return;
    CHECK(f.cursor.storage.allocated && probe.constructed == 1 && probe.resumed == 0);
    CHECK(isTaskGenerator(task.context(), f.cursor));
    if (captureScope)
      kalandra_file_scope_leave_v1(&task.context(), captureScope, &f.cursor);
    f.pc = probe.mode == 9 ? 4 : 1;
  }
  f.itemScope.reset();
  Invocation::OwnershipSelection selected(f.itemScope);
  if (f.pc == 4) {
    if (!generatorTaskClose(task, f.cursor))
      return;
    CHECK(generatorTaskNext(task, f.cursor, coordinatingValue, &f.value));
    CHECK(f.value.step == GeneratorStep::Exhausted && probe.resumed == 0);
    const std::int64_t result = 17;
    task.complete(&result);
    return;
  }
  if (f.pc == 1) {
    if (!generatorTaskNext(task, f.cursor, coordinatingValue, &f.value))
      return;
    CHECK(f.value.step == GeneratorStep::Item &&
          std::string_view(f.value.text.data, f.value.text.byteLength) == "first");
    ++probe.items;
    probe.first.store(true, std::memory_order_release);
    f.pc = 2;
  }
  if (!probe.advance.load(std::memory_order_acquire)) {
    task.yield();
    return;
  }
  if (f.pc == 2) {
    if (probe.mode == 3) {
      const std::int64_t result = 17;
      task.complete(&result); // abandonment must run the same owned cleanup
      return;
    }
    if (probe.mode == 2) {
      if (!generatorTaskClose(task, f.cursor))
        return;
    } else {
      if (!generatorTaskNext(task, f.cursor, coordinatingValue, &f.value))
        return;
      if (probe.mode == 10 && f.value.step == GeneratorStep::Item) {
        CHECK(std::string_view(f.value.text.data, f.value.text.byteLength) == "first");
        ++probe.items;
        task.yield();
        return;
      }
      if (probe.mode == 11)
        probe.terminalPublished.store(true, std::memory_order_release);
      CHECK(f.value.step == (probe.mode == 1 ? GeneratorStep::Error : GeneratorStep::Exhausted));
      if (probe.mode == 1)
        CHECK(std::string_view(f.value.text.data, f.value.text.byteLength) == "error");
    }
    CHECK(probe.destroyed == 1);
    f.pc = 3;
  }
  CHECK(generatorTaskNext(task, f.cursor, coordinatingValue, &f.value));
  CHECK(f.value.step == GeneratorStep::Exhausted); // sticky, no body re-entry
  CHECK(generatorTaskClose(task, f.cursor));
  const std::int64_t result = 17;
  task.complete(&result);
}
const TaskEntry coordinatingConsumer{1, 0, sizeof(CoordinatingProbe *), alignof(CoordinatingProbe *),
                                    sizeof(CoordinatingConsumer), alignof(CoordinatingConsumer),
                                    constructCoordinatingConsumer, destroyCoordinatingConsumer,
                                    resumeCoordinatingConsumer, &integer};
struct CaptureFailureConsumer {
  CoordinatingProbe *outer, *inner;
  GeneratorDescriptor cursor;
  CoordinatingValue value;
};
bool constructCaptureFailureConsumer(Invocation &, void *out, const Packet &packet) {
  new (out) CaptureFailureConsumer(*static_cast<const CaptureFailureConsumer *>(packet.data));
  return true;
}
void resumeCaptureFailureConsumer(Task &task) {
  auto &f = *static_cast<CaptureFailureConsumer *>(task.frame());
  if (!f.cursor.storage.allocated) {
    auto *input = f.inner;
    f.cursor = generatorTaskCreate(task, coordinatingGenerator,
                                   {0, &input, sizeof(input), &task.context()});
    CHECK(f.cursor.storage.allocated);
  }
  if (!f.inner->first.load(std::memory_order_acquire)) {
    if (!generatorTaskNext(task, f.cursor, coordinatingValue, &f.value))
      return;
    CHECK(f.value.step == GeneratorStep::Item);
    f.inner->first.store(true, std::memory_order_release);
  }
  if (!f.inner->advance.load(std::memory_order_acquire)) {
    task.yield();
    return;
  }
  f.outer->captureSource = &task.context();
  f.outer->captured = f.cursor;
  auto *input = f.outer;
  auto failed = generatorTaskCreate(task, coordinatingGenerator,
                                    {0, &input, sizeof(input), &task.context()});
  CHECK(!failed.storage.allocated);
  CHECK(task.context().status() == (f.outer->mode == 7 ? 2 : 3));
}
const TaskEntry captureFailureConsumer{1, 0, sizeof(CaptureFailureConsumer), alignof(CaptureFailureConsumer),
                                     sizeof(CaptureFailureConsumer), alignof(CaptureFailureConsumer),
                                     constructCaptureFailureConsumer, nothing,
                                     resumeCaptureFailureConsumer, &integer};
void partialCoordinatingCapture() {
  for (unsigned workers : {1u, 4u})
    for (unsigned mode : {6u, 7u}) {
      TaskLimits limits;
      limits.workers = workers;
      limits.records = 2; // one consumer and one real child; no cursor task
      limits.groups = 2;
      limits.ready = 1;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      CoordinatingProbe outer, inner;
      outer.mode = mode;
      CaptureFailureConsumer input{&outer, &inner, {}, {}};
      auto task = core->submit(group, captureFailureConsumer, {0, &input, sizeof(input)});
      CHECK(task);
      for (unsigned i = 0; i != 1024 &&
           !(inner.first.load(std::memory_order_acquire) &&
             inner.externalReady.load(std::memory_order_acquire)); ++i)
        core->step();
      CHECK(inner.first && inner.externalReady && inner.resumed == 1 && inner.destroyed == 0);
      inner.advance.store(true, std::memory_order_release);
      for (unsigned i = 0; i != 256; ++i)
        core->step();
      CHECK(outer.constructed == 1 && outer.resumed == 0 && outer.destroyed == 0);
      CHECK(!core->settled(task) && inner.destroyed == 0 && inner.childDestroyed == 0);
      CHECK(inner.completion.signal());
      CHECK(core->drainToCompletion() && core->settled(task));
      const auto failure = core->failure(task);
      CHECK(failure.kind == TaskFailure::Fault && failure.code == (mode == 7 ? 2 : 3));
      CHECK(inner.destroyed == 1 && outer.destroyed == 0);
      CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
      CHECK(core->stats().retainedBytes == 0 && core->stats().callBytes == 0 &&
            core->stats().records == 0 && core->stats().retainedOwners == 0);
    }
}
void cancellationDuringGeneratorStaging() {
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 2;
    limits.groups = 2;
    limits.ready = 1;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    CoordinatingProbe probe;
    probe.mode = 8;
    probe.cancelCore = core.get();
    probe.cancelGroup = group;
    auto *input = &probe;
    auto task = core->submit(group, coordinatingConsumer, {0, &input, sizeof(input)});
    CHECK(task);
    for (unsigned i = 0; i != 512 && !core->settled(task); ++i)
      core->step();
    if (probe.externalReady.load(std::memory_order_acquire))
      CHECK(probe.completion.signal());
    CHECK(core->drainToCompletion() && core->settled(task));
    CHECK(core->failure(task).kind == TaskFailure::Cancelled);
    CHECK(probe.copies == 1 && probe.destroyed == 1 && probe.resumed == 1);
    CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
    CHECK(core->stats().retainedBytes == 0 && core->stats().callBytes == 0 &&
          core->stats().records == 0);
  }
}
void boundedCoordinatingStream() {
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 2;
    limits.groups = 2;
    limits.ready = 1;
    limits.bytes = 65536;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    CoordinatingProbe probe;
    probe.mode = 10;
    probe.advance = true;
    auto *input = &probe;
    auto task = core->submit(group, coordinatingConsumer, {0, &input, sizeof(input)});
    CHECK(task);
    for (unsigned i = 0; i != 100000 && probe.items != 10000 && !core->settled(task); ++i)
      core->step();
    CHECK(probe.items == 10000 && !core->settled(task));
    for (unsigned i = 0; i != 128; ++i)
      core->step();
    CHECK(probe.destroyed == 0 && probe.childDestroyed == 0 && probe.externalReady);
    CHECK(probe.completion.signal());
    CHECK(core->drainToCompletion() && core->settled(task));
    CHECK(core->failure(task).kind == TaskFailure::None && probe.resumed == 10001 && probe.destroyed == 1);
    Invocation receiver;
    std::int64_t value = 0;
    CHECK(core->observe(task, integer, receiver, &value) && value == 17);
    CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
    CHECK(core->stats().retainedBytes == 0 && core->stats().peakBytes <= limits.bytes &&
          core->stats().callBytes == 0);
  }
}
void coordinatingGenerators() {
  for (unsigned workers : {1u, 4u})
    for (bool captureBuffer : {false, true})
      for (unsigned mode : {0u, 1u, 2u, 3u, 4u, 5u, 9u, 11u}) {
        TaskLimits limits;
        limits.workers = workers;
        limits.records = 2; // one consumer and one real child; no cursor task
        limits.groups = 2;
        limits.ready = 1;
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        CoordinatingProbe probe;
        probe.mode = mode;
        probe.captureBuffer = captureBuffer;
        auto *input = &probe;
        auto task = core->submit(group, coordinatingConsumer, {0, &input, sizeof(input)});
        CHECK(task);
        if (mode != 5 && mode != 9) {
          for (unsigned i = 0; i != 1024 && !(probe.first.load(std::memory_order_acquire) &&
                                              probe.externalReady.load(std::memory_order_acquire));
               ++i)
            core->step();
          CHECK(probe.first && probe.externalReady && probe.resumed == 1 && probe.destroyed == 0 &&
                probe.childDestroyed == 0);
          CHECK(core->stats().records == 2 && core->stats().callFrames == 0);
          probe.advance.store(true, std::memory_order_release);
          for (unsigned i = 0; i != 256; ++i)
            core->step();
          CHECK(!core->settled(task) && probe.destroyed == 0 && probe.childDestroyed == 0);
          CHECK(probe.completion.signal());
        }
        CHECK(core->drainToCompletion() && core->settled(task));
        const auto failure = core->failure(task);
        if (mode == 11) {
          CHECK(failure.kind == TaskFailure::Fault && failure.code == 9 && failure.line == 44 &&
                failure.column == 2);
          CHECK(!probe.terminalPublished.load(std::memory_order_acquire));
        } else if (mode == 4)
          CHECK(failure.kind == TaskFailure::Fault && failure.code == 8 && failure.line == 37);
        else if (mode == 5)
          CHECK(failure.kind == TaskFailure::Fault && failure.code == 3);
        else {
          CHECK(failure.kind == TaskFailure::None);
          Invocation receiver;
          std::int64_t value = 0;
          CHECK(core->observe(task, integer, receiver, &value) && value == 17);
        }
        CHECK(probe.constructed == 1 && probe.destroyed == unsigned(mode != 5));
        CHECK(probe.resumed == (mode == 5 || mode == 9 ? 0 : mode == 2 || mode == 3 ? 1 : 2));
        CHECK(core->close(group) && core->drainToCompletion() && core->release(group));
        CHECK(core->stats().retainedBytes == 0 && core->stats().callBytes == 0 &&
              core->stats().records == 0 && core->stats().retainedOwners == 0);
      }
}
} // namespace
int main() {
  coordinatingGenerators();
  partialCoordinatingCapture();
  cancellationDuringGeneratorStaging();
  boundedCoordinatingStream();
  multipleFrameLedgers();
  independent();
  failures();
  bounded();
  ownerBudgets();
  pausedResourceGrowth();
  deepResourceBudgets();
  sharedResourceBudgets();
  activeClose();
  typedTextFailure();
  outlivesCreator();
  invalid();
  rangeSources();
  numericArraySources<kalandra::NumericType::Int8>();
  numericArraySources<kalandra::NumericType::Int16>();
  numericArraySources<kalandra::NumericType::Int32>();
  numericArraySources<kalandra::NumericType::Int64>();
  numericArraySources<kalandra::NumericType::UInt8>();
  numericArraySources<kalandra::NumericType::UInt16>();
  numericArraySources<kalandra::NumericType::UInt32>();
  numericArraySources<kalandra::NumericType::UInt64>();
  numericArraySources<kalandra::NumericType::Float32>();
  numericArraySources<kalandra::NumericType::Float64>();
  arrayBackingLifetime();
  booleanArraySource();
  arraySourceFailures();
  retirementDuringPublication();
  std::puts("PASS: lazy owned frames, typed terminal results, capture lifetime and bounds");
}
