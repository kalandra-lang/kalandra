#include "Async.h"
#include "../runtime/NumericStorage.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
namespace kalandra::runtime::vulkan {
struct AsyncRuntime::State {
  struct Work {
    void *frame = nullptr;
    ExternalCompletion completion;
  };
  std::shared_ptr<Runtime> runtime;
  AsyncLimits limits;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::vector<Work> queue;
  std::size_t head = 0, tail = 0, count = 0;
  AsyncStats stats;
  bool stopping = false, hold = false;
  std::atomic<bool> fail{false};
  std::thread worker;
  ~State() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    changed.notify_all();
    if (worker.joinable())
      worker.join();
  }
  bool reserve(std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    if (stats.pending >= limits.maxPending || bytes > limits.maxCaptureBytes - stats.captureBytes)
      return false;
    ++stats.pending;
    stats.captureBytes += bytes;
    return true;
  }
  void release(std::uint64_t bytes) {
    std::lock_guard lock(mutex);
    --stats.pending;
    stats.captureBytes -= bytes;
  }
};
struct AsyncRuntime::Bridge {
  struct Reservation {
    std::shared_ptr<State> state;
    std::uint64_t bytes = 0;
    bool held = false;
    ~Reservation() {
      if (held)
        state->release(bytes);
    }
  };
  struct Operation {
    // First member, last destroyed: retire buffers, parameters and invocation
    // state before returning their metadata/admission reservation to the pool.
    Reservation reservation;
    std::shared_ptr<State> state;
    Kernel kernel;
    std::vector<Buffer> inputs;
    std::vector<std::uint32_t> parameters;
    std::uint64_t outputBytes = 0;
    std::uint32_t groups = 0;
    std::optional<std::uint32_t> faultWord;
    std::optional<ProgramExecution> program;
    std::optional<NumericType> scalarType{};
  };
  struct PacketData {
    std::shared_ptr<Operation> operation;
  };
  struct Frame {
    std::shared_ptr<Operation> operation;
    std::shared_ptr<State> owner;
    Buffer output{};
    std::optional<Error> error{};
    bool started = false;
    TensorValue tensorOutput{};
    bool tensorResult = false;
    std::optional<NumericType> scalarType{};
    alignas(std::max_align_t) std::array<std::byte, 8> scalarOutput{};
  };
  static bool construct(Invocation &, void *memory, const Packet &packet) {
    auto &data = *static_cast<const PacketData *>(packet.data);
    auto *frame = new (memory) Frame{data.operation, data.operation->state};
    frame->tensorResult = data.operation->program.has_value();
    frame->scalarType = data.operation->scalarType;
    return true;
  }
  static void destroy(void *memory) noexcept { static_cast<Frame *>(memory)->~Frame(); }
  static bool copy(Invocation &, void *out, const Invocation &, const void *input) noexcept {
    new (out) Buffer(*static_cast<const Buffer *>(input));
    return true;
  }
  static void destroyBuffer(void *out) noexcept { static_cast<Buffer *>(out)->~Buffer(); }
  static bool copyTensor(Invocation &, void *out, const Invocation &, const void *input) noexcept {
    new (out) TensorValue(*static_cast<const TensorValue *>(input));
    return true;
  }
  static void destroyTensor(void *out) noexcept { static_cast<TensorValue *>(out)->~TensorValue(); }
  static const ValueLayout layout;
  static const ValueLayout tensorLayout;
  static const TaskEntry entry, programEntry;
  static const std::array<TaskEntry, 10> scalarEntries, scalarFaultEntries;
  static const TaskEntry &scalarEntry(NumericType type, bool fault = false) {
    for (std::size_t i = 0; i < numericTypes.size(); ++i)
      if (numericTypes[i].type == type)
        return fault ? scalarFaultEntries[i] : scalarEntries[i];
    return tensorFaultEntry;
  }
  static void resume(Task &task) {
    auto &frame = *static_cast<Frame *>(task.frame());
    if (task.cancelled())
      return;
    if (frame.started) {
      if (frame.error)
        task.fault(static_cast<std::int64_t>(*frame.error));
      else if (frame.scalarType)
        task.complete(frame.scalarOutput.data());
      else if (frame.tensorResult)
        task.complete(&frame.tensorOutput);
      else
        task.complete(&frame.output);
      return;
    }
    auto completion = task.suspendExternal();
    if (!completion)
      return;
    frame.started = true;
    auto &state = *frame.operation->state;
    bool queued = false;
    {
      std::lock_guard lock(state.mutex);
      // Every captured operation already owns a maxPending reservation.
      if (state.count == state.queue.size() || state.stopping)
        frame.error = Error::Budget;
      else {
        state.queue[state.tail] = {&frame, completion};
        state.tail = (state.tail + 1) % state.queue.size();
        ++state.count;
        ++state.stats.queued;
        queued = true;
      }
    }
    if (!queued) {
      frame.operation.reset();
      completion.signal();
      return;
    }
    state.changed.notify_one();
  }
  static void run(State &state) noexcept {
    for (;;) {
      State::Work work;
      {
        std::unique_lock lock(state.mutex);
        state.changed.wait(lock, [&] { return state.stopping || state.count; });
        if (state.stopping && !state.count)
          return;
        work = state.queue[state.head];
        state.queue[state.head] = {};
        state.head = (state.head + 1) % state.queue.size();
        --state.count;
        --state.stats.queued;
        ++state.stats.running;
      }
      auto &frame = *static_cast<Frame *>(work.frame);
      auto &operation = *frame.operation;
      bool reschedule = false, stage = false;
      if (!work.completion.cancelled()) {
        if (operation.program) {
          auto command = operation.program->next(*state.runtime);
          if (!command)
            frame.error = command.error();
          else if (*command) {
            stage = true;
            auto &dispatch = **command;
            auto result =
                state.runtime->execute(dispatch.kernel, dispatch.inputs, dispatch.parameters,
                                       dispatch.outputBytes, dispatch.groups);
            if (!result)
              frame.error = result.error();
            else {
              auto accepted = operation.program->accept(*state.runtime, std::move(*result));
              if (!accepted)
                frame.error = accepted.error();
              else
                reschedule = true;
            }
          } else if (operation.program->finished()) {
            auto result = operation.program->result();
            if (!result)
              frame.error = result.error();
            else if (operation.scalarType) {
              auto copied = state.runtime->readBytes(
                  result->data, std::span(frame.scalarOutput)
                                    .first(describeNumeric(*operation.scalarType)->bytes()));
              if (!copied)
                frame.error = copied.error();
            } else
              frame.tensorOutput = std::move(*result);
          } else
            reschedule = true;
        } else {
          stage = true;
          auto result =
              state.runtime->execute(operation.kernel, operation.inputs, operation.parameters,
                                     operation.outputBytes, operation.groups);
          if (!result)
            frame.error = result.error();
          else if (operation.faultWord && result->parameters[*operation.faultWord] != UINT32_MAX)
            frame.error = Error::Arithmetic;
          else
            frame.output = std::move(result->output);
        }
        if (stage && state.fail.exchange(false))
          frame.error = Error::Device;
      }
      {
        std::unique_lock lock(state.mutex);
        if (state.hold) {
          ++state.stats.held;
          state.changed.wait(lock, [&] { return !state.hold || state.stopping; });
          --state.stats.held;
        }
        --state.stats.running;
        state.stats.stages += stage;
      }
      // A program owns one external capability and task record throughout.
      // Round-robin continuation tickets permit cancellation and other admitted
      // invocations between every source-control step or completed GPU stage.
      // Never take the TaskCore lock while holding the completion-service lock.
      if (reschedule && !frame.error && !work.completion.cancelled()) {
        bool queued = false;
        {
          std::lock_guard lock(state.mutex);
          if (state.count < state.queue.size() && !state.stopping) {
            state.queue[state.tail] = work;
            state.tail = (state.tail + 1) % state.queue.size();
            ++state.count;
            ++state.stats.queued;
            queued = true;
          } else
            frame.error = Error::Budget;
        }
        if (queued)
          continue;
      }
      {
        std::lock_guard lock(state.mutex);
        ++state.stats.completed;
      }
      // GPU fences have completed. Retire captures before acknowledgement, but
      // keep the output and status in the externally pinned frame for resume.
      frame.operation.reset();
      work.completion.signal();
      // Never touch frame/operation after signal: the scheduler may destroy
      // them immediately. The worker owns no State shared_ptr; its last owner
      // can therefore shut down/join the worker from a controller safely.
    }
  }
  struct FaultFrame {
    Error error;
  };
  static bool faultConstruct(Invocation &, void *out, const Packet &packet) {
    new (out) FaultFrame{*static_cast<const Error *>(packet.data)};
    return true;
  }
  static void faultDestroy(void *out) noexcept { static_cast<FaultFrame *>(out)->~FaultFrame(); }
  static void faultResume(Task &task) {
    task.fault(static_cast<std::int64_t>(static_cast<FaultFrame *>(task.frame())->error));
  }
  static const TaskEntry faultEntry, tensorFaultEntry;
  template <class Caller>
  static TaskHandle submit(Caller &caller, GroupHandle group, const TaskEntry &descriptor,
                           Packet packet) {
    if constexpr (std::is_same_v<Caller, TaskCore>)
      return caller.submit(group, descriptor, packet);
    else
      return caller.spawn(group, descriptor, packet);
  }
  template <class Caller>
  static TaskHandle rejected(Caller &caller, GroupHandle group, Error error, bool tensor = false,
                             std::optional<NumericType> scalar = {}) {
    const auto &descriptor = scalar   ? scalarEntry(*scalar, true)
                             : tensor ? tensorFaultEntry
                                      : faultEntry;
    return submit(caller, group, descriptor, {descriptor.packetLayout, &error, sizeof(error)});
  }
  template <class Caller>
  static TaskHandle execute(Caller &caller, GroupHandle group, std::shared_ptr<State> state,
                            Kernel kernel, std::span<const Buffer> inputs,
                            std::span<const std::uint32_t> parameters, std::uint64_t outputBytes,
                            std::uint32_t groups, std::optional<std::uint32_t> faultWord) noexcept {
    if (!kernel || parameters.empty() || (faultWord && *faultWord >= parameters.size()))
      return rejected(caller, group, Error::Invalid);
    if (inputs.size() > 128 || parameters.size() > 1024 * 1024 / 4)
      return rejected(caller, group, Error::Budget);
    try {
      auto operation = std::make_shared<Operation>();
      operation->state = std::move(state);
      // Includes both the captured words and the driver's returned fault words.
      operation->reservation.state = operation->state;
      operation->reservation.bytes =
          inputs.size_bytes() + 2 * parameters.size_bytes() + sizeof(Operation);
      if (!operation->state->reserve(operation->reservation.bytes))
        return rejected(caller, group, Error::Budget);
      operation->reservation.held = true;
      operation->kernel = std::move(kernel);
      operation->inputs.assign(inputs.begin(), inputs.end());
      operation->parameters.assign(parameters.begin(), parameters.end());
      operation->outputBytes = outputBytes;
      operation->groups = groups;
      operation->faultWord = faultWord;
      PacketData packet{std::move(operation)};
      return submit(caller, group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    } catch (const std::bad_alloc &) {
      return rejected(caller, group, Error::Allocation);
    }
  }
  template <class Caller>
  static TaskHandle executeProgram(Caller &caller, GroupHandle group, std::shared_ptr<State> state,
                                   const Program &program, std::span<const TensorValue> inputs,
                                   std::span<const std::uint64_t> scalars) noexcept {
    const auto scalarType = program.scalarType();
    auto bytes = program.invocationBytes(scalars.size());
    if (!bytes)
      return rejected(caller, group, bytes.error(), true, scalarType);
    if (*bytes > UINT64_MAX - sizeof(Operation))
      return rejected(caller, group, Error::Budget, true, scalarType);
    try {
      auto operation = std::make_shared<Operation>();
      operation->state = std::move(state);
      operation->reservation.state = operation->state;
      operation->reservation.bytes = *bytes + sizeof(Operation);
      if (!operation->state->reserve(operation->reservation.bytes))
        return rejected(caller, group, Error::Budget, true, scalarType);
      operation->reservation.held = true;
      auto invocation = program.start(*operation->state->runtime, inputs, scalars);
      if (!invocation)
        return rejected(caller, group, invocation.error(), true, scalarType);
      operation->program.emplace(std::move(*invocation));
      operation->scalarType = scalarType;
      PacketData packet{std::move(operation)};
      const auto &descriptor = scalarType ? scalarEntry(*scalarType) : programEntry;
      return submit(caller, group, descriptor, {descriptor.packetLayout, &packet, sizeof(packet)});
    } catch (const std::bad_alloc &) {
      return rejected(caller, group, Error::Allocation, true, scalarType);
    }
  }
};
const ValueLayout AsyncRuntime::Bridge::layout{0x564b42554601, sizeof(Buffer), alignof(Buffer),
                                               copy, destroyBuffer};
const ValueLayout AsyncRuntime::Bridge::tensorLayout{
    0x564b54454e01, sizeof(TensorValue), alignof(TensorValue), copyTensor, destroyTensor};
const TaskEntry AsyncRuntime::Bridge::entry{1,
                                            0x564b42554602,
                                            sizeof(PacketData),
                                            alignof(PacketData),
                                            sizeof(Frame),
                                            alignof(Frame),
                                            construct,
                                            destroy,
                                            resume,
                                            &layout};
const TaskEntry AsyncRuntime::Bridge::faultEntry{1,
                                                 0x564b42554603,
                                                 sizeof(Error),
                                                 alignof(Error),
                                                 sizeof(FaultFrame),
                                                 alignof(FaultFrame),
                                                 faultConstruct,
                                                 faultDestroy,
                                                 faultResume,
                                                 &layout};
const TaskEntry AsyncRuntime::Bridge::programEntry{1,
                                                   0x564b54454e02,
                                                   sizeof(PacketData),
                                                   alignof(PacketData),
                                                   sizeof(Frame),
                                                   alignof(Frame),
                                                   construct,
                                                   destroy,
                                                   resume,
                                                   &tensorLayout};
const TaskEntry AsyncRuntime::Bridge::tensorFaultEntry{1,
                                                       0x564b54454e03,
                                                       sizeof(Error),
                                                       alignof(Error),
                                                       sizeof(FaultFrame),
                                                       alignof(FaultFrame),
                                                       faultConstruct,
                                                       faultDestroy,
                                                       faultResume,
                                                       &tensorLayout};
const std::array<TaskEntry, 10> AsyncRuntime::Bridge::scalarEntries = [] {
  std::array<TaskEntry, 10> entries{};
  for (std::size_t i = 0; i < entries.size(); ++i)
    entries[i] = {1,
                  0x564b5343411000 + i,
                  sizeof(PacketData),
                  alignof(PacketData),
                  sizeof(Frame),
                  alignof(Frame),
                  construct,
                  destroy,
                  resume,
                  numericScalarLayout(numericTypes[i].type)};
  return entries;
}();
const std::array<TaskEntry, 10> AsyncRuntime::Bridge::scalarFaultEntries = [] {
  std::array<TaskEntry, 10> entries{};
  for (std::size_t i = 0; i < entries.size(); ++i)
    entries[i] = {1,
                  0x564b5343412000 + i,
                  sizeof(Error),
                  alignof(Error),
                  sizeof(FaultFrame),
                  alignof(FaultFrame),
                  faultConstruct,
                  faultDestroy,
                  faultResume,
                  numericScalarLayout(numericTypes[i].type)};
  return entries;
}();
std::unique_ptr<AsyncRuntime> AsyncRuntime::create(std::shared_ptr<Runtime> runtime,
                                                   AsyncLimits limits) noexcept {
  if (!runtime || !limits.maxPending || limits.maxPending > 4096 || !limits.maxCaptureBytes)
    return {};
  try {
    auto state = std::make_shared<State>();
    state->runtime = std::move(runtime);
    state->limits = limits;
    state->queue.resize(limits.maxPending);
    auto result = std::unique_ptr<AsyncRuntime>(new AsyncRuntime(state));
    state->worker = std::thread([pointer = state.get()] { Bridge::run(*pointer); });
    return result;
  } catch (...) {
    return {};
  }
}
AsyncRuntime::~AsyncRuntime() = default;
const ValueLayout &AsyncRuntime::bufferLayout() noexcept { return Bridge::layout; }
const ValueLayout &AsyncRuntime::tensorLayout() noexcept { return Bridge::tensorLayout; }
AsyncStats AsyncRuntime::stats() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->stats;
}
void AsyncRuntime::holdCompletions(bool hold) noexcept {
  {
    std::lock_guard lock(state_->mutex);
    state_->hold = hold;
  }
  state_->changed.notify_all();
}
void AsyncRuntime::failNextCompletion() noexcept { state_->fail = true; }
TaskHandle AsyncRuntime::execute(TaskCore &core, GroupHandle group, Kernel kernel,
                                 std::span<const Buffer> inputs,
                                 std::span<const std::uint32_t> parameters,
                                 std::uint64_t outputBytes, std::uint32_t groups,
                                 std::optional<std::uint32_t> faultWord) noexcept {
  return Bridge::execute(core, group, state_, std::move(kernel), inputs, parameters, outputBytes,
                         groups, faultWord);
}
TaskHandle AsyncRuntime::execute(Task &task, GroupHandle group, Kernel kernel,
                                 std::span<const Buffer> inputs,
                                 std::span<const std::uint32_t> parameters,
                                 std::uint64_t outputBytes, std::uint32_t groups,
                                 std::optional<std::uint32_t> faultWord) noexcept {
  return Bridge::execute(task, group, state_, std::move(kernel), inputs, parameters, outputBytes,
                         groups, faultWord);
}
TaskHandle AsyncRuntime::executeProgram(TaskCore &core, GroupHandle group, const Program &program,
                                        std::span<const TensorValue> inputs,
                                        std::span<const std::uint64_t> scalars) noexcept {
  return Bridge::executeProgram(core, group, state_, program, inputs, scalars);
}
TaskHandle AsyncRuntime::executeProgram(Task &task, GroupHandle group, const Program &program,
                                        std::span<const TensorValue> inputs,
                                        std::span<const std::uint64_t> scalars) noexcept {
  return Bridge::executeProgram(task, group, state_, program, inputs, scalars);
}
} // namespace kalandra::runtime::vulkan
