#include "GeneratorTasks.h"
#include <cstdlib>
#include <optional>
#include <utility>
extern "C" bool kalandra_source_run_v3(kalandra::runtime::Invocation *, bool (*)(void *),
                                       void *) noexcept;
namespace kalandra::runtime {
namespace {
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
bool alignment(std::size_t n) noexcept {
  return n && n <= alignof(std::max_align_t) && !(n & (n - 1));
}
struct alignas(std::max_align_t) GeneratorTaskState : OwnedState {
  Invocation storage, staged;
  BorrowedGroupOwner groups{storage};
  const GeneratorTaskEntry entry;
  TaskEntry wrapper;
  BorrowedTaskEntry borrowed;
  BorrowedCleanup retirement;
  std::optional<Invocation::OwnershipScope> stagedScope;
  std::atomic_flag advancing = ATOMIC_FLAG_INIT;
  std::atomic_flag retiringPayload = ATOMIC_FLAG_INIT;
  std::atomic<bool> ended{false};
  std::size_t resultOffset;
  bool constructed = false, stagedValue = false, terminal = false;
  GeneratorTaskState(const GeneratorTaskEntry &entry, std::size_t bytes,
                     std::size_t offset) noexcept
      : OwnedState(OwnedKind::Generator, bytes, destroyState, retireState), entry(entry),
        wrapper(*entry.body),
        borrowed{&wrapper, &storage, this + 1, this, enter, leave, &groups, true, stage},
        retirement{this, children, finish, failure}, resultOffset(offset) {
    wrapper.construct = nullptr;
    wrapper.destroy = nullptr;
    wrapper.resume = resume;
    deferredOwner = &groups;
    deferredCleanup = &retirement;
  }
  void *frame() noexcept { return this + 1; }
  void *result() noexcept { return reinterpret_cast<std::byte *>(this) + resultOffset; }
  void clearStaged() noexcept {
    if (std::exchange(stagedValue, false))
      entry.body->result->destroy(result());
    if (stagedScope)
      stagedScope->reset();
  }
  void finishPayload() noexcept {
    if (retiringPayload.test_and_set(std::memory_order_acquire))
      return;
    if (std::exchange(constructed, false))
      entry.body->destroy(frame());
    storage.settle();
    ended.store(true, std::memory_order_release);
  }
  ~GeneratorTaskState() {
    // All source owners preflight retirement before releasing the header. A
    // standalone foreign Invocation destructor is not a scheduler barrier.
    finishPayload();
    clearStaged();
    stagedScope.reset();
    staged.settle();
  }
  static void destroyState(void *p) noexcept {
    static_cast<GeneratorTaskState *>(p)->~GeneratorTaskState();
  }
  static void retireState(void *p) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    if (s.groups.cleanupFinished())
      s.finishPayload();
  }
  static bool enter(void *p, Invocation &caller) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    if (s.advancing.test_and_set(std::memory_order_acquire))
      return false;
    s.storage.bindCancellation(InvocationABI::cancellationToken(caller));
    s.staged.bindCancellation(InvocationABI::cancellationToken(caller));
    return true;
  }
  static void leave(void *p) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    // Failure maintenance has already joined groups and retired body scopes.
    // Never make a partially executed, cancelled producer resumable again.
    if (!s.ended.load(std::memory_order_acquire) && !healthy(s.storage))
      s.finishPayload();
    s.clearStaged();
    s.storage.bindCancellation(nullptr);
    s.staged.bindCancellation(nullptr);
    s.advancing.clear(std::memory_order_release);
  }
  static void children(BorrowedGroupOwner &owner, void *p) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    if (!owner.enqueueOwnedScopeChildren(s.storage))
      std::abort(); // compiler/runtime ownership invariant, no user callback
  }
  static void finish(void *p) noexcept { static_cast<GeneratorTaskState *>(p)->finishPayload(); }
  static Failure failure(void *p) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    return s.storage.status() ? Failure{TaskFailure::Fault, s.storage.status(), s.storage.line(),
                                        s.storage.column()}
                              : Failure{};
  }
  static void stage(Task &task, void *p, const Invocation &source, const void *value) noexcept {
    auto &s = *static_cast<GeneratorTaskState *>(p);
    if (s.stagedValue || !s.stagedScope || !s.stagedScope->valid()) {
      task.context().fault(3);
      return;
    }
    Invocation::OwnershipSelection selected(*s.stagedScope);
    if (!s.entry.body->result->copy(s.staged, s.result(), source, value)) {
      if (s.staged.status())
        task.context().fault(s.staged.status(), s.staged.line(), s.staged.column());
      else if (!task.cancelled())
        task.context().fault(3);
      return;
    }
    s.stagedValue = true;
  }
  static void resume(Task &task) {
    auto &s = *static_cast<GeneratorTaskState *>(task.borrowedState());
    if (s.terminal) {
      if (!s.groups.cleanupFinished()) {
        task.context().fault(3);
        return;
      }
      const auto failure = task.ownedCleanupFailure();
      if (failure.kind == TaskFailure::Fault) {
        task.fail(failure); // a child fault wins before the staged terminal value is observable
        return;
      }
      task.completeFrom(s.staged, s.result(), true);
      return;
    }
    s.entry.body->resume(task);
    if (!s.stagedValue)
      return; // the body suspended in a call, await, timer or explicit yield
    const auto step = s.entry.classify(s.result());
    if (step == GeneratorStep::Item) {
      task.completeFrom(s.staged, s.result(), true);
      return;
    }
    if (step != GeneratorStep::Exhausted && step != GeneratorStep::Error) {
      task.context().fault(3);
      return;
    }
    s.terminal = true;
    task.beginBorrowedCleanup(s.retirement);
  }
};
GeneratorTaskState *inspect(Invocation &ctx, const GeneratorDescriptor &value) noexcept {
  auto *state = inspectOwned(ctx, value);
  if (!state)
    return nullptr;
  if (state->kind != OwnedKind::Generator || state->bytes < sizeof(GeneratorTaskState) ||
      state->destroy != GeneratorTaskState::destroyState) {
    ctx.fault(3);
    return nullptr;
  }
  return static_cast<GeneratorTaskState *>(state);
}
struct Construction {
  GeneratorTaskState &state;
  const Packet &packet;
  static bool run(void *p) {
    auto &call = *static_cast<Construction *>(p);
    call.state.constructed =
        call.state.entry.body->construct(call.state.storage, call.state.frame(), call.packet);
    return call.state.constructed;
  }
};
} // namespace
GeneratorDescriptor generatorTaskCreate(Task &task, const GeneratorTaskEntry &entry,
                                        const Packet &packet) noexcept {
  auto &ctx = task.context();
  if (!healthy(ctx))
    return {};
  const auto *body = entry.body;
  if (entry.revision != 1 || !body || body->revision != 1 || !body->construct || !body->destroy ||
      !body->resume || !body->frameBytes || !alignment(body->frameAlignment) ||
      !alignment(body->packetAlignment) || !body->result || !body->result->identity ||
      !body->result->bytes || !alignment(body->result->alignment) || !body->result->copy ||
      !body->result->destroy || !entry.classify || !entry.exhausted ||
      packet.layout != body->packetLayout || packet.bytes != body->packetBytes ||
      (packet.bytes &&
       (!packet.data || reinterpret_cast<std::uintptr_t>(packet.data) % body->packetAlignment))) {
    ctx.fault(3);
    return {};
  }
  constexpr auto align = alignof(std::max_align_t);
  constexpr auto maximum = std::size_t(INT64_MAX);
  if (body->frameBytes > maximum - sizeof(GeneratorTaskState) - align ||
      body->result->bytes > maximum - sizeof(GeneratorTaskState) - align - body->frameBytes) {
    ctx.fault(2);
    return {};
  }
  const auto offset = (sizeof(GeneratorTaskState) + body->frameBytes + align - 1) & ~(align - 1);
  if (!ensureOwnedEscapeScope(ctx))
    return {};
  auto *scope = currentScope(ctx);
  Invocation::OwnershipSelection selected(scope->storage);
  auto *memory = ctx.allocateResource(offset + body->result->bytes, OwnedState::cleanup, true);
  if (!memory)
    return {};
  auto *state = new (memory) GeneratorTaskState(entry, offset + body->result->bytes, offset);
  if (!ctx.trackResource(state))
    return {};
  adopt(ctx, *state, scope->scope); // admit a logical owner before captures can move
  if (!healthy(ctx))
    return {};
  state->storage.bindResourceBudget(ctx, state);
  state->staged.bindResourceBudget(ctx, state);
  if (!healthy(state->storage) || !healthy(state->staged)) {
    ctx.fault(2);
    return {};
  }
  state->stagedScope.emplace(state->staged);
  if (!state->stagedScope->valid() || !ensureOwnedEscapeScope(state->storage)) {
    ctx.fault(state->storage.status() ? state->storage.status() : 2);
    return {};
  }
  state->storage.bindCancellation(InvocationABI::cancellationToken(ctx));
  Construction construction{*state, packet};
  bool complete;
  {
    // These guards must live outside the native interruption boundary: a
    // cancellation transfer can leave the generated constructor immediately.
    auto *captureScope = currentScope(state->storage);
    Invocation::OwnershipSelection captures(captureScope->storage);
    std::optional<OwnedReceiveGuard> receiving;
    if (packet.source)
      receiving.emplace(*packet.source, state->storage);
    complete = kalandra_source_run_v3(&state->storage, Construction::run, &construction);
  }
  state->storage.bindCancellation(nullptr);
  if (!complete || !healthy(state->storage) || !healthy(ctx)) {
    if (state->storage.status())
      ctx.fault(state->storage.status(), state->storage.line(), state->storage.column());
    else if (!ctx.poll())
      ctx.fault(3);
    // The admitted source owner retires partial captures through its task's
    // failure barrier. Do not synchronously settle the new producer here.
    return {};
  }
  return ownedDescriptor(state);
}
bool isTaskGenerator(Invocation &ctx, const GeneratorDescriptor &descriptor) noexcept {
  auto *state = inspectOwned(ctx, descriptor);
  return state && state->kind == OwnedKind::Generator &&
         state->destroy == GeneratorTaskState::destroyState;
}
bool generatorTaskNext(Task &task, const GeneratorDescriptor &descriptor, const ValueLayout &result,
                       void *output) noexcept {
  auto &ctx = task.context();
  if (!healthy(ctx))
    return false;
  auto *state = inspect(ctx, descriptor);
  if (!state)
    return false;
  const auto &expected = *state->entry.body->result;
  if (result.identity != expected.identity || result.bytes != expected.bytes ||
      result.alignment != expected.alignment || !output ||
      reinterpret_cast<std::uintptr_t>(output) % result.alignment) {
    ctx.fault(3);
    return false;
  }
  if (state->ended.load(std::memory_order_acquire) &&
      !state->advancing.test(std::memory_order_acquire)) {
    state->entry.exhausted(output);
    return true;
  }
  auto pin = ctx.lease(state);
  if (!pin) {
    ctx.fault(3);
    return false;
  }
  return task.callBorrowed(state->borrowed, std::move(*pin), output);
}
bool generatorTaskClose(Task &task, const GeneratorDescriptor &descriptor) noexcept {
  auto &ctx = task.context();
  if (!healthy(ctx))
    return false;
  auto *state = inspect(ctx, descriptor);
  if (!state)
    return false;
  if (state->advancing.test(std::memory_order_acquire)) {
    ctx.fault(3);
    return false;
  }
  if (state->groups.cleanupFinished()) {
    const auto failure = task.ownedCleanupFailure();
    if (failure.kind == TaskFailure::Fault)
      ctx.fault(failure.code, failure.line, failure.column);
    return true;
  }
  auto pin = ctx.lease(state);
  if (!pin) {
    ctx.fault(3);
    return false;
  }
  const auto started = task.beginOwnedCleanup(state->groups, std::move(*pin), state->retirement);
  if (started == CleanupStart::Complete)
    return true;
  return false;
}
} // namespace kalandra::runtime
extern "C" {
void kalandra_generator_task_create_v1(kalandra::runtime::Task *task,
                                       kalandra::runtime::GeneratorDescriptor *output,
                                       const kalandra::runtime::GeneratorTaskEntry *entry,
                                       const kalandra::runtime::Packet *packet) noexcept {
  *output = kalandra::runtime::generatorTaskCreate(*task, *entry, *packet);
}
bool kalandra_generator_task_kind_v1(kalandra::runtime::Invocation *context,
                                     const kalandra::runtime::GeneratorDescriptor *value) noexcept {
  return kalandra::runtime::isTaskGenerator(*context, *value);
}
bool kalandra_generator_task_next_v1(kalandra::runtime::Task *task,
                                     const kalandra::runtime::GeneratorDescriptor *value,
                                     const kalandra::runtime::ValueLayout *result,
                                     void *output) noexcept {
  return kalandra::runtime::generatorTaskNext(*task, *value, *result, output);
}
bool kalandra_generator_task_close_v1(
    kalandra::runtime::Task *task, const kalandra::runtime::GeneratorDescriptor *value) noexcept {
  return kalandra::runtime::generatorTaskClose(*task, *value);
}
}
