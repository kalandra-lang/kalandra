#include "Async.h"
#include "SanitizerMaps.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  check(bool(value), message);
  return std::move(*value);
}
struct Hold {
  v::AsyncRuntime &runtime;
  explicit Hold(v::AsyncRuntime &r) : runtime(r) { runtime.holdCompletions(true); }
  ~Hold() { runtime.holdCompletions(false); }
};
void held(TaskCore &core, v::AsyncRuntime &runtime) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!runtime.stats().held && std::chrono::steady_clock::now() < end) {
    core.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(runtime.stats().held == 1, "completion must reach held gate");
}
v::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(v::Buffer) std::byte bytes[sizeof(v::Buffer)];
  check(core.observe(task, v::AsyncRuntime::bufferLayout(), context, bytes),
        "observe completed buffer");
  auto *value = std::launder(reinterpret_cast<v::Buffer *>(bytes));
  auto copy = *value;
  value->~Buffer();
  return copy;
}
void release(TaskCore &core, GroupHandle group) {
  check(core.drain(), "drain");
  check(core.release(group), "release group");
  check(!core.stats().records && !core.stats().externalPending && !core.stats().retainedBytes,
        "task owner cleanup");
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 5)
    return 2;
  bool validation = true, maps = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--maps")
      maps = true;
    else if (std::string_view(argv[i]) == "--no-validation")
      validation = false;
    else
      return 2;
  }
  std::shared_ptr<v::Runtime> runtime =
      take(v::Runtime::create(
               {.validation = validation,
                .deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0}),
           "create driver");
  auto kernel = take(runtime->loadKernel(argv[1]), "load actual compiler kernel");
  auto left = take(runtime->upload(std::array{1.0f, 2.0f, 3.0f}), "left");
  auto right = take(runtime->upload(std::array{4.0f, 5.0f, 6.0f}), "right");
  std::array inputs{left, right};
  std::array parameters{3u, UINT32_MAX};
  for (const auto &words :
       {std::vector<std::uint32_t>{3}, std::vector<std::uint32_t>{3, UINT32_MAX, 0}}) {
    auto invalid = runtime->execute(kernel, inputs, words, 12, 1);
    check(!invalid && invalid.error() == v::Error::Invalid,
          "short and extra parameter carriers rejected before dispatch");
  }
  v::Buffer escaped;
  for (unsigned workers : {1u, 2u, 4u}) {
    auto async = v::AsyncRuntime::create(runtime);
    check(bool(async), "create completion service");
    auto core = TaskCore::create({.workers = workers});
    check(bool(core), "create core");
    auto group = core->openRoot();
    auto task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
    check(bool(task) && core->close(group) && core->drain(), "managed compute");
    check(core->failure(task).kind == TaskFailure::None, "managed success");
    escaped = observe(*core, task);
    auto cached = observe(*core, task);
    check(take(runtime->download(cached), "cached output") == std::vector<float>({5, 12, 21}),
          "exact cached result");
    release(*core, group);
    check(!async->stats().pending && !async->stats().captureBytes, "capture ledger retired");
    // Captures must own their buffers and copy carrier words at submission;
    // callers can discard all their aliases before the task starts.
    group = core->openRoot();
    {
      auto local = take(runtime->upload(std::array{2.0f, 3.0f, 4.0f}), "capture input");
      std::array captured{local, local};
      std::array words{3u, UINT32_MAX};
      task = async->execute(*core, group, kernel, captured, words, 12, 1, 1);
      captured = {};
      local = {};
      words = {0u, 0u};
    }
    check(bool(task) && core->close(group) && core->drain(), "owned captures complete");
    check(core->failure(task).kind == TaskFailure::None, "capture words copied");
    check(take(runtime->download(observe(*core, task)), "owned capture result") ==
              std::vector<float>({6, 12, 20}),
          "buffer captures survive caller alias teardown");
    release(*core, group);
    // Cancellation/deadline waits for acknowledgement; the scheduler remains
    // able to process cancellation while the completion thread is held.
    for (bool deadline : {false, true}) {
      group = core->openRoot();
      Hold hold(*async);
      task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
      check(bool(task) && core->close(group), "held submission");
      held(*core, *async);
      auto retained = runtime->retainedBytes();
      check(core->cancel(group, deadline) && !core->settled(group),
            "cancellation retains pending external access");
      check(core->stats().externalPending == 1 && runtime->retainedBytes() == retained,
            "pending resources not reclaimed");
      async->holdCompletions(false);
      check(core->drain(), "cancelled drain");
      check(core->failure(group).kind ==
                (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
            "cancel/deadline precedence");
      release(*core, group);
    }
    group = core->openRoot();
    async->failNextCompletion();
    task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
    check(bool(task) && core->close(group) && core->drain(), "device failure drain");
    check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 105,
          "device failure stays Native");
    release(*core, group);
    group = core->openRoot();
    {
      Hold hold(*async);
      async->failNextCompletion();
      task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
      check(bool(task) && core->close(group), "raced failure submission");
      held(*core, *async);
      check(core->cancel(group), "raced cancellation");
      async->holdCompletions(false);
      check(core->drain(), "raced drain");
      check(core->failure(task).kind == TaskFailure::Cancelled,
            "recorded cancellation wins completion fault");
    }
    release(*core, group);
    // Dropping the facade cannot destroy the service while a captured frame
    // still owns it, even when the frame has not yet been resumed/submitted.
    group = core->openRoot();
    task = async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1);
    check(bool(task) && core->close(group), "orphaned facade submission");
    async.reset();
    check(core->drain() && core->failure(task).kind == TaskFailure::None,
          "facade-independent completion");
    release(*core, group);
  }
  {
    auto async = v::AsyncRuntime::create(runtime, {.maxPending = 1});
    auto core = TaskCore::create();
    check(bool(async) && bool(core), "bounded setup");
    Hold hold(*async);
    auto a = core->openRoot();
    auto first = async->execute(*core, a, kernel, inputs, parameters, 12, 1, 1);
    check(bool(first) && core->close(a), "first bounded task");
    held(*core, *async);
    auto b = core->openRoot();
    auto second = async->execute(*core, b, kernel, inputs, parameters, 12, 1, 1);
    check(bool(second) && core->close(b), "budget rejection is a structured fault task");
    for (unsigned i = 0; i < 100 && !core->settled(second); ++i)
      core->step();
    check(core->failure(second).kind == TaskFailure::Fault && core->failure(second).code == 6,
          "bounded admission fault");
    async->holdCompletions(false);
    check(core->drain(), "bounded drain");
    check(core->release(a) && core->release(b), "bounded group cleanup");
    check(!async->stats().pending && !async->stats().captureBytes, "bounded captures reclaimed");
  }
  {
    auto async = v::AsyncRuntime::create(runtime);
    auto core = TaskCore::create();
    check(bool(async) && bool(core), "teardown setup");
    Hold hold(*async);
    auto group = core->openRoot();
    check(bool(async->execute(*core, group, kernel, inputs, parameters, 12, 1, 1)),
          "teardown submit");
    held(*core, *async);
    std::atomic<bool> destroyed = false;
    std::thread destroyer([&] {
      core.reset();
      destroyed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool waited = !destroyed.load();
    async->holdCompletions(false);
    destroyer.join();
    check(waited && destroyed, "core destruction awaits safe acknowledgement");
    check(!async->stats().pending && !async->stats().captureBytes, "teardown captures reclaimed");
  }
  check(take(runtime->download(escaped), "escaped outcome") == std::vector<float>({5, 12, 21}),
        "outcome outlives producing core/facade");
  escaped = {};
  inputs = {};
  left = {};
  right = {};
  check(runtime->retainedBytes() == 0 && runtime->validationErrors() == 0,
        "complete device cleanup and validation");
  if (maps)
    check(writeSanitizerMaps(), "capture loaded module maps");
  std::printf("PASS: Vulkan managed tasks, cached ownership, cancellation/deadlines, faults, "
              "bounds and teardown on %s (Vulkan validation %s)\n",
              runtime->device().name.c_str(), validation ? "enabled" : "disabled");
  return 0;
}
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
