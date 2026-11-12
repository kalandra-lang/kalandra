#include "GeneratorFrame.h"
#include <algorithm>
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
  const GeneratorEntry &entry;
  Invocation *active = nullptr;
  std::atomic_flag advancing = ATOMIC_FLAG_INIT;
  std::uint64_t retained = 0;
  std::size_t resultOffset;
  bool constructed = false, ended = false;
  GeneratorState(const GeneratorEntry &entry, std::size_t size, std::size_t resultOffset) noexcept
      : OwnedState(OwnedKind::Generator, size, destroyState, retireState), entry(entry),
        resultOffset(resultOffset) {
    // The frame resource's dynamic accounting follows all of its actual owners.
    // Never keep a creator TaskRecord's budget or cancellation pointer alive in
    // a dormant cursor. An active caller is borrowed only for a resume/construct.
    storage.bindRecursiveBudget(this, reserve, release);
  }
  void *frame() noexcept { return this + 1; }
  void *result() noexcept { return reinterpret_cast<std::byte *>(this) + resultOffset; }
  static bool reserve(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<GeneratorState *>(p);
    if (!s.active || bytes > UINT64_MAX - s.retained ||
        !s.active->resizeResource(&s, s.retained + bytes))
      return false;
    s.retained += bytes;
    return true;
  }
  static void release(void *p, std::uint64_t bytes) noexcept {
    auto &s = *static_cast<GeneratorState *>(p);
    s.retained -= bytes;
  }
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
    state.active = &caller;
    state.storage.bindCancellation(InvocationABI::cancellationToken(caller));
  }
  ~Active() {
    state.storage.bindCancellation(nullptr);
    state.active = nullptr;
  }
};
struct Advance {
  GeneratorState &state;
  bool acquired;
  explicit Advance(GeneratorState &s) noexcept
      : state(s), acquired(!s.advancing.test_and_set(std::memory_order_acquire)) {}
  ~Advance() {
    if (acquired)
      state.advancing.clear(std::memory_order_release);
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
  Advance access(*state);
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
  if (!publish || (produced && !copied)) {
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
  Advance access(*state);
  if (!access.acquired) {
    ctx.fault(3);
    return false;
  }
  state->finish();
  return true;
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
