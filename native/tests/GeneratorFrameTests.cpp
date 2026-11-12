#include "../runtime/GeneratorFrame.h"
#include "../runtime/String.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
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
  static bool reserve(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<Budget *>(p);
    if (bytes > s.maximum - s.bytes)
      return false;
    s.bytes += bytes;
    return true;
  }
  static void release(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<Budget *>(p);
    CHECK(bytes <= s.bytes);
    s.bytes -= bytes;
  }
};
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
} // namespace
int main() {
  independent();
  failures();
  bounded();
  ownerBudgets();
  activeClose();
  typedTextFailure();
  outlivesCreator();
  invalid();
  std::puts("PASS: lazy owned frames, typed terminal results, capture lifetime and bounds");
}
