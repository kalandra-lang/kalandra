#include "Async.h"
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
  struct Operation {
    std::shared_ptr<State> state;
    Kernel kernel;
    std::vector<Buffer> inputs;
    std::vector<std::uint32_t> parameters;
    std::uint64_t outputBytes = 0, reservedBytes = 0;
    std::uint32_t groups = 0;
    std::optional<std::uint32_t> faultWord;
    bool reserved = false;
    ~Operation() { release(); }
    void release() {
      if (reserved) {
        state->release(reservedBytes);
        reserved = false;
      }
    }
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
  };
  static bool construct(Invocation &, void *memory, const Packet &packet) {
    auto &data = *static_cast<const PacketData *>(packet.data);
    new (memory) Frame{data.operation, data.operation->state};
    return true;
  }
  static void destroy(void *memory) noexcept { static_cast<Frame *>(memory)->~Frame(); }
  static bool copy(Invocation &, void *out, const Invocation &, const void *input) noexcept {
    new (out) Buffer(*static_cast<const Buffer *>(input));
    return true;
  }
  static void destroyBuffer(void *out) noexcept { static_cast<Buffer *>(out)->~Buffer(); }
  static const ValueLayout layout;
  static const TaskEntry entry;
  static void resume(Task &task) {
    auto &frame = *static_cast<Frame *>(task.frame());
    if (task.cancelled())
      return;
    if (frame.started) {
      if (frame.error)
        task.fault(static_cast<std::int64_t>(*frame.error));
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
      if (!work.completion.cancelled()) {
        auto result =
            state.runtime->execute(operation.kernel, operation.inputs, operation.parameters,
                                   operation.outputBytes, operation.groups);
        if (state.fail.exchange(false))
          frame.error = Error::Device;
        else if (!result)
          frame.error = result.error();
        else if (operation.faultWord && result->parameters[*operation.faultWord] != UINT32_MAX)
          frame.error = Error::Arithmetic;
        else
          frame.output = std::move(result->output);
      }
      {
        std::unique_lock lock(state.mutex);
        if (state.hold) {
          ++state.stats.held;
          state.changed.wait(lock, [&] { return !state.hold || state.stopping; });
          --state.stats.held;
        }
        --state.stats.running;
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
  static const TaskEntry faultEntry;
  template <class Caller>
  static TaskHandle submit(Caller &caller, GroupHandle group, const TaskEntry &descriptor,
                           Packet packet) {
    if constexpr (std::is_same_v<Caller, TaskCore>)
      return caller.submit(group, descriptor, packet);
    else
      return caller.spawn(group, descriptor, packet);
  }
  template <class Caller>
  static TaskHandle rejected(Caller &caller, GroupHandle group, Error error) {
    return submit(caller, group, faultEntry, {faultEntry.packetLayout, &error, sizeof(error)});
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
      operation->reservedBytes =
          inputs.size_bytes() + 2 * parameters.size_bytes() + sizeof(Operation);
      if (!operation->state->reserve(operation->reservedBytes))
        return rejected(caller, group, Error::Budget);
      operation->reserved = true;
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
};
const ValueLayout AsyncRuntime::Bridge::layout{0x564b42554601, sizeof(Buffer), alignof(Buffer),
                                               copy, destroyBuffer};
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
} // namespace kalandra::runtime::vulkan
