#include "GeneratorFrame.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
extern "C" bool kalandra_source_run_v3(kalandra::runtime::Invocation *, bool (*)(void *),
                                       void *) noexcept;
namespace kalandra::runtime {
namespace {
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
bool layout(const ValueLayout &value) noexcept {
  return value.identity && value.bytes && value.copy && value.destroy && value.alignment &&
         value.alignment <= alignof(std::max_align_t) &&
         !(value.alignment & (value.alignment - 1));
}
bool same(const ValueLayout &left, const ValueLayout &right) noexcept {
  return left.identity == right.identity && left.bytes == right.bytes &&
         left.alignment == right.alignment;
}
struct alignas(std::max_align_t) GeneratorState : OwnedState {
  Invocation storage;
  const GeneratorEntry entry;
  std::atomic_flag advancing = ATOMIC_FLAG_INIT;
  std::size_t resultOffset;
  bool constructed = false, ended = false;
  GeneratorState(const GeneratorEntry &entry, std::size_t size, std::size_t resultOffset) noexcept
      : OwnedState(OwnedKind::Generator, size, destroyState, retireState), entry(entry),
        resultOffset(resultOffset) {}
  void *frame() noexcept { return this + 1; }
  void *result() noexcept { return reinterpret_cast<std::byte *>(this) + resultOffset; }
  void finish() noexcept {
    if (std::exchange(ended, true))
      return;
    if (std::exchange(constructed, false))
      entry.destroy(frame());
    storage.settle();
  }
  ~GeneratorState() { finish(); }
  static void destroyState(void *p) noexcept { static_cast<GeneratorState *>(p)->~GeneratorState(); }
  static void retireState(void *p) noexcept {
    auto &s = *static_cast<GeneratorState *>(p);
    if (!s.advancing.test_and_set(std::memory_order_acquire)) {
      s.finish();
      s.advancing.clear(std::memory_order_release);
    }
    // An active resume observes OwnedState::retired and performs its own
    // cleanup before releasing its execution lease. Never destroy live locals.
  }
};
GeneratorState *inspect(Invocation &ctx, const GeneratorDescriptor &value) noexcept {
  auto *state = inspectOwned(ctx, value);
  if (!state)
    return nullptr;
  if (state->kind != OwnedKind::Generator || state->bytes < sizeof(GeneratorState)) {
    ctx.fault(3);
    return nullptr;
  }
  return static_cast<GeneratorState *>(state);
}
void stopped(Invocation &caller, GeneratorState &state) noexcept {
  if (auto code = state.storage.status())
    caller.fault(code, state.storage.line(), state.storage.column());
  else if (state.storage.hasExpectedError())
    // Expected errors use the typed entry result, never an untyped side channel.
    caller.fault(3);
}
struct Active {
  GeneratorState &state;
  Active(GeneratorState &state, Invocation &caller) noexcept : state(state) {
    state.storage.bindCancellation(InvocationABI::cancellationToken(caller));
  }
  ~Active() { state.storage.bindCancellation(nullptr); }
};
struct Advance {
  GeneratorState &state;
  // An owner scope may disappear during publication. The execution borrower
  // pins physical state until after releasing the advancing/retirement handoff.
  AllocationLease pin;
  bool acquired;
  Advance(GeneratorState &s, AllocationLease lease) noexcept
      : state(s), pin(std::move(lease)),
        acquired(!s.advancing.test_and_set(std::memory_order_acquire)) {}
  ~Advance() {
    if (!acquired)
      return;
    state.advancing.clear(std::memory_order_release);
    // Retirement may have observed the busy flag after resume's last check.
    // Releasing before testing closes that handoff race: either retirement
    // acquires the idle frame, or the final active borrower performs cleanup.
    if (state.retired.load(std::memory_order_acquire) &&
        !state.advancing.test_and_set(std::memory_order_acquire)) {
      state.finish();
      state.advancing.clear(std::memory_order_release);
    }
  }
};
struct Construction {
  GeneratorState &state;
  const Packet &packet;
  static bool run(void *p) {
    auto &call = *static_cast<Construction *>(p);
    call.state.constructed = call.state.entry.construct(call.state.storage, call.state.frame(),
                                                       call.packet);
    return call.state.constructed;
  }
};
struct Resume {
  GeneratorState &state;
  GeneratorStep step = GeneratorStep::Stopped;
  static bool run(void *p) {
    auto &call = *static_cast<Resume *>(p);
    call.step = call.state.entry.resume(call.state.storage, call.state.frame(), call.state.result());
    return true;
  }
};
constexpr auto rangePacket = UINT64_C(0x4b414c52414e4745);
bool constructRange(Invocation &ctx, void *out, const Packet &input) {
  if (input.layout != rangePacket || input.bytes != sizeof(IntegerRangeValue) || !input.data) {
    ctx.fault(3);
    return false;
  }
  new (out) IntegerRangeValue(*static_cast<const IntegerRangeValue *>(input.data));
  return true;
}
void destroyRange(void *) noexcept {}
GeneratorStep resumeRange(Invocation &, void *frame, void *out) {
  auto &range = *static_cast<IntegerRangeValue *>(frame);
  if (range.step > 0 ? range.start >= range.stop : range.start <= range.stop)
    return GeneratorStep::Exhausted;
  *static_cast<std::int64_t *>(out) = range.start;
  std::int64_t advanced;
  range.start = __builtin_add_overflow(range.start, range.step, &advanced) ? range.stop : advanced;
  return GeneratorStep::Item;
}
// Copy the common rank-one descriptor without aliasing another element type.
struct ArrayValue {
  void *allocated, *aligned;
  std::int64_t offset, count, stride;
};
static_assert(sizeof(ArrayValue) == 40 && alignof(ArrayValue) == 8);
struct ArrayCapture {
  const ValueLayout *source, *item;
  const void *descriptor;
};
struct ArrayFrame {
  const ValueLayout *source;
  ArrayValue value;
  std::uint64_t bytes, position = 0;
};
constexpr auto arrayPacket = UINT64_C(0x4b414c4152524159);
bool constructArray(Invocation &ctx, void *out, const Packet &input) {
  if (input.layout != arrayPacket || input.bytes != sizeof(ArrayCapture) || !input.data ||
      !input.source) {
    ctx.fault(3);
    return false;
  }
  const auto &capture = *static_cast<const ArrayCapture *>(input.data);
  auto &frame = *new (out) ArrayFrame{capture.source, {}, capture.item->bytes};
  if (!capture.source->copy(ctx, &frame.value, *input.source, capture.descriptor))
    return false;
  const auto fail = [&] {
    capture.source->destroy(&frame.value);
    ctx.fault(3);
    return false;
  };
  const auto &value = frame.value;
  if (value.offset < 0 || value.count < 0 || value.stride != 1)
    return fail();
  if (value.count) {
    const auto base = reinterpret_cast<std::uintptr_t>(value.allocated);
    const auto aligned = reinterpret_cast<std::uintptr_t>(value.aligned);
    auto lease = ctx.lease(value.allocated);
    if (!lease || aligned < base || aligned % capture.item->alignment)
      return fail();
    const auto available = lease->bytes().size();
    const auto shift = aligned - base;
    const auto end = std::uint64_t(value.offset) + std::uint64_t(value.count);
    if (shift > available || end > (available - shift) / frame.bytes)
      return fail();
  }
  return true;
}
void destroyArray(void *p) noexcept {
  auto &frame = *static_cast<ArrayFrame *>(p);
  frame.source->destroy(&frame.value);
  frame.~ArrayFrame();
}
GeneratorStep resumeArray(Invocation &, void *p, void *out) {
  auto &frame = *static_cast<ArrayFrame *>(p);
  if (frame.position == std::uint64_t(frame.value.count))
    return GeneratorStep::Exhausted;
  const auto index = std::uint64_t(frame.value.offset) + frame.position++;
  std::memcpy(out, static_cast<const std::byte *>(frame.value.aligned) + index * frame.bytes,
              frame.bytes);
  return GeneratorStep::Item;
}
} // namespace
GeneratorDescriptor generatorCreate(Invocation &ctx, const GeneratorEntry &entry,
                                      const Packet &packet) noexcept {
  if (!healthy(ctx))
    return {};
  if (entry.revision != 1 || !entry.frameBytes || !entry.frameAlignment ||
      entry.frameAlignment > alignof(GeneratorState) ||
      (entry.frameAlignment & (entry.frameAlignment - 1)) || !entry.construct || !entry.destroy ||
      !entry.resume || !entry.item || !layout(*entry.item) ||
      (entry.error && !layout(*entry.error))) {
    ctx.fault(3);
    return {};
  }
  constexpr auto alignment = alignof(std::max_align_t);
  constexpr auto maximum = std::size_t(INT64_MAX);
  const auto resultBytes = std::max(entry.item->bytes, entry.error ? entry.error->bytes : 0);
  if (entry.frameBytes > maximum - sizeof(GeneratorState) - alignment ||
      resultBytes > maximum - sizeof(GeneratorState) - alignment - entry.frameBytes) {
    ctx.fault(2);
    return {};
  }
  const auto offset = (sizeof(GeneratorState) + entry.frameBytes + alignment - 1) & ~(alignment - 1);
  const auto size = offset + resultBytes;
  auto *memory = ctx.allocateResource(size, OwnedState::cleanup, true);
  if (!memory)
    return {};
  auto *state = new (memory) GeneratorState(entry, size, offset);
  if (!ctx.trackResource(state))
    return {};
  state->storage.bindResourceBudget(ctx, state);
  if (state->storage.status()) {
    stopped(ctx, *state);
    return {};
  }
  {
    Active active(*state, ctx);
    Construction call{*state, packet};
    const bool complete = kalandra_source_run_v3(&state->storage, Construction::run, &call);
    if (!complete || !healthy(state->storage) || !healthy(ctx)) {
      stopped(ctx, *state);
      if (!ctx.poll())
        ctx.fault(3);
      state->finish();
      return {};
    }
  }
  if (auto *scope = currentScope(ctx))
    adopt(ctx, *state, scope->scope);
  return healthy(ctx) ? ownedDescriptor(state) : GeneratorDescriptor{};
}
GeneratorStep generatorNext(Invocation &ctx, const GeneratorDescriptor &descriptor,
                            const ValueLayout &item, const ValueLayout *error, void *output,
                            void *failure) noexcept {
  if (!healthy(ctx))
    return GeneratorStep::Stopped;
  auto *state = inspect(ctx, descriptor);
  if (!state)
    return GeneratorStep::Stopped;
  if (!same(item, *state->entry.item) || bool(error) != bool(state->entry.error) ||
      (error && !same(*error, *state->entry.error)) || !output || (error && !failure)) {
    ctx.fault(3);
    return GeneratorStep::Stopped;
  }
  auto pin = ctx.lease(state);
  if (!pin) {
    ctx.fault(3);
    return GeneratorStep::Stopped;
  }
  Advance access(*state, std::move(*pin));
  if (!access.acquired) {
    ctx.fault(3);
    return GeneratorStep::Stopped;
  }
  if (state->ended || state->retired.load(std::memory_order_acquire))
    return GeneratorStep::Exhausted;
  Active active(*state, ctx);
  Resume call{*state};
  const bool complete = kalandra_source_run_v3(&state->storage, Resume::run, &call);
  const ValueLayout *produced = call.step == GeneratorStep::Item ? state->entry.item
                                : call.step == GeneratorStep::Error ? state->entry.error
                                                                  : nullptr;
  const bool publish = complete && healthy(state->storage) && healthy(ctx) &&
                       !state->retired.load(std::memory_order_acquire);
  bool copied = false;
  if (produced) {
    if (publish)
      copied = produced->copy(ctx, call.step == GeneratorStep::Item ? output : failure,
                              state->storage, state->result());
    produced->destroy(state->result());
  }
  if (!publish || (produced && !copied) || !healthy(ctx) || !healthy(state->storage) ||
      state->retired.load(std::memory_order_acquire)) {
    stopped(ctx, *state);
    if (!ctx.poll())
      ctx.fault(3);
    state->finish();
    return GeneratorStep::Stopped;
  }
  if (call.step == GeneratorStep::Exhausted || call.step == GeneratorStep::Error) {
    if (call.step == GeneratorStep::Error && !produced) {
      ctx.fault(3);
      state->finish();
      return GeneratorStep::Stopped;
    }
    state->finish();
  } else if (call.step != GeneratorStep::Item) {
    ctx.fault(3);
    state->finish();
    return GeneratorStep::Stopped;
  }
  return call.step;
}
bool generatorClose(Invocation &ctx, const GeneratorDescriptor &descriptor) noexcept {
  auto *state = inspect(ctx, descriptor);
  if (!state)
    return false;
  auto pin = ctx.lease(state);
  if (!pin) {
    ctx.fault(3);
    return false;
  }
  Advance access(*state, std::move(*pin));
  if (!access.acquired) {
    ctx.fault(3);
    return false;
  }
  state->finish();
  return true;
}
GeneratorDescriptor generatorRange(Invocation &ctx, IntegerRangeValue range,
                                   const ValueLayout &item, std::int64_t line,
                                   std::int64_t column) noexcept {
  if (!healthy(ctx))
    return {};
  if (!layout(item) || item.bytes != sizeof(std::int64_t) ||
      item.alignment != alignof(std::int64_t)) {
    ctx.fault(3);
    return {};
  }
  if (!range.step) {
    ctx.fault(4, line, column);
    return {};
  }
  const GeneratorEntry entry{1,
                             sizeof(IntegerRangeValue),
                             alignof(IntegerRangeValue),
                             constructRange,
                             destroyRange,
                             resumeRange,
                             &item,
                             nullptr};
  return generatorCreate(ctx, entry, {rangePacket, &range, sizeof(range), &ctx});
}
GeneratorDescriptor generatorArray(Invocation &ctx, const ValueLayout &source,
                                   const ValueLayout &item, const void *descriptor) noexcept {
  if (!healthy(ctx))
    return {};
  if (!layout(source) || !layout(item) || source.bytes != sizeof(ArrayValue) ||
      source.alignment != alignof(ArrayValue) || !descriptor) {
    ctx.fault(3);
    return {};
  }
  const ArrayCapture capture{&source, &item, descriptor};
  const GeneratorEntry entry{1,
                             sizeof(ArrayFrame),
                             alignof(ArrayFrame),
                             constructArray,
                             destroyArray,
                             resumeArray,
                             &item,
                             nullptr};
  return generatorCreate(ctx, entry, {arrayPacket, &capture, sizeof(capture), &ctx});
}
} // namespace kalandra::runtime
using namespace kalandra::runtime;
extern "C" void kalandra_generator_create_v1(Invocation *ctx, GeneratorDescriptor *out,
                                              const GeneratorEntry *entry,
                                              const Packet *packet) noexcept {
  if (!out || !entry || !packet) {
    ctx->fault(3);
    return;
  }
  *out = generatorCreate(*ctx, *entry, *packet);
}
extern "C" std::uint32_t kalandra_generator_next_v1(Invocation *ctx,
                                                     const GeneratorDescriptor *cursor,
                                                     const ValueLayout *item,
                                                     const ValueLayout *error, void *output,
                                                     void *failure) noexcept {
  if (!cursor || !item) {
    ctx->fault(3);
    return std::uint32_t(GeneratorStep::Stopped);
  }
  return std::uint32_t(generatorNext(*ctx, *cursor, *item, error, output, failure));
}
extern "C" bool kalandra_generator_close_v1(Invocation *ctx,
                                             const GeneratorDescriptor *cursor) noexcept {
  if (!cursor) {
    ctx->fault(3);
    return false;
  }
  return generatorClose(*ctx, *cursor);
}
extern "C" void kalandra_iterator_range_v1(Invocation *ctx, GeneratorDescriptor *out,
                                           const IntegerRangeValue *range, const ValueLayout *item,
                                           std::int64_t line, std::int64_t column) noexcept {
  if (!out || !range || !item) {
    ctx->fault(3);
    return;
  }
  *out = generatorRange(*ctx, *range, *item, line, column);
}

extern "C" void kalandra_iterator_array_v1(Invocation *ctx, GeneratorDescriptor *out,
                                           const ValueLayout *source, const ValueLayout *item,
                                           const void *input) noexcept {
  if (!out || !source || !item || !input) {
    ctx->fault(3);
    return;
  }
  *out = generatorArray(*ctx, *source, *item, input);
}
