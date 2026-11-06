// Exercise the actual compiler-generated closure submission, ownership policies
// and shaders. Replace only its CLI, leaving source execution unchanged.
#define main kalandra_generated_cli
#include "host.cpp"
#undef main
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
std::uint64_t nativeFailure = 0;
bool observedConstruct(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(nativeFailure);
  return entry.construct(context, frame, packet);
}
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void empty(m::Runtime &runtime, TaskCore &core) {
  const auto s = runtime.stats();
  const auto t = core.stats();
  require(!s.bytes && !s.inFlight && !s.argumentBytes &&
              !t.records && !t.groups && !t.externalPending && !t.retainedBytes &&
              !t.retainedOwners && !t.environmentBytes && !t.recursiveBytes,
          "settled closure task retained resources");
  const auto backend = [](const auto &stats) {
    if constexpr (requires { stats.metalBuffers; })
      require(!stats.metalBuffers && !stats.hostBackings && !stats.deviceBackings &&
                  !stats.stagingBuffers && !stats.argumentPacks,
              "settled closure task retained Metal backing");
  };
  backend(s);
}
enum class Mode { Success, PreCancel, PreDeadline, HeldCancel, HeldDeadline,
                  NativeAllocation, DeviceAllocation, Admission, Completion };
std::uint64_t run(const char *directory, unsigned workers, Mode mode, unsigned attempt = 0,
         unsigned concurrent = 1) {
  auto created = m::Runtime::create();
  require(bool(created), "create closure-test runtime");
  auto runtime = std::move(*created);
  TaskLimits limits;
  limits.workers = workers;
  auto observed = entry;
  observed.construct = observedConstruct;
  PacketData packet{runtime.get(), directory};
  auto core = TaskCore::create(limits);
  require(bool(core), "create closure-test scheduler");
  nativeFailure = mode == Mode::NativeAllocation ? attempt : 0;
  const bool held = mode == Mode::HeldCancel || mode == Mode::HeldDeadline;
  const auto hold = [&](auto &backend, bool enabled) {
    if constexpr (requires { backend.holdCompletionAt(0); })
      backend.holdCompletionAt(enabled ? attempt : 0);
    else
      backend.holdCompletions(enabled);
  };
  if (held)
    hold(*runtime, true);
  if (mode == Mode::DeviceAllocation)
    runtime->failAllocationAt(attempt);
  if (mode == Mode::Completion)
    runtime->failNextCompletion();
  const auto group = core->openRoot();
  require(bool(group), "create closure-test group");
  if (mode == Mode::Admission)
    core->failAllocationAt(attempt);
  std::vector<TaskHandle> tasks;
  for (unsigned i = 0; i < concurrent; ++i)
    tasks.push_back(core->submit(group, observed,
                                {observed.packetLayout, &packet, sizeof(packet), nullptr}));
  if (mode == Mode::PreCancel || mode == Mode::PreDeadline)
    require(core->cancel(group, mode == Mode::PreDeadline), "pre-cancel closure task");
  if (held) {
    bool reached = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      core->step();
      if (core->stats().externalPending && runtime->stats().inFlight &&
          runtime->stats().submissions >= attempt) {
        reached = true;
        break;
      }
      if (core->failure(group).kind != TaskFailure::None)
        break;
      std::this_thread::yield();
    }
    if (reached)
      core->cancel(group, mode == Mode::HeldDeadline);
    // Always acknowledge held work before assertions or frame destruction.
    hold(*runtime, false);
    require(reached, "closure task never reached a held GPU completion");
  }
  require(core->drain(), "drain closure-test group");
  auto failure = core->failure(group);
  for (auto task : tasks) {
    const auto child = task ? core->failure(task) : failure;
    if (failure.kind == TaskFailure::None && child.kind != TaskFailure::None)
      failure = child;
    if (mode == Mode::Success)
      require(task && child.kind == TaskFailure::None, "concurrent closure execution failed");
    if (child.kind == TaskFailure::None && task) {
      Invocation output;
      std::int64_t value = 0;
      require(core->observe(task, *entry.result, output, &value) && value == 32,
              "closure tensor result changed");
      output.settle();
      require(!output.live(), "closure result observation retained resources");
    }
  }
  if (mode == Mode::PreCancel || mode == Mode::HeldCancel)
    require(failure.kind == TaskFailure::Cancelled, "closure cancellation changed precedence");
  if (mode == Mode::PreDeadline || mode == Mode::HeldDeadline)
    require(failure.kind == TaskFailure::DeadlineExceeded, "closure deadline changed precedence");
  if (mode == Mode::Completion)
    require(failure.kind == TaskFailure::Fault, "device completion failure was hidden");
  if (mode == Mode::NativeAllocation || mode == Mode::DeviceAllocation || mode == Mode::Admission)
    require(failure.kind == TaskFailure::None || failure.kind == TaskFailure::AdmissionRejected ||
                (failure.kind == TaskFailure::Fault && failure.code == 2),
            "closure allocation failure changed fault kind");
  if ((mode == Mode::NativeAllocation || mode == Mode::DeviceAllocation || mode == Mode::Admission) &&
      attempt == 1)
    require(failure.kind != TaskFailure::None, "first injected allocation failure did not execute");
  if (failure.kind == TaskFailure::Fault) {
    core->cancel(group);
    require(core->failure(group).kind == failure.kind && core->failure(group).code == failure.code,
            "later cancellation replaced an earlier closure fault");
  }
  require(core->close(group) && core->drain() && core->release(group),
          "release closure-test group");
  empty(*runtime, *core);
  return runtime->stats().submissions;
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected closure shader directory");
    for (unsigned workers : {1u, 4u}) {
      run(argv[1], workers, Mode::Success, 0, 4);
      const auto submissions = run(argv[1], workers, Mode::Success);
      require(submissions >= 3, "closure fixture omitted transport or device execution");
      const auto holds = []<class Backend>(std::uint64_t count) {
        if constexpr (requires(Backend &backend) { backend.holdCompletionAt(0); })
          return count;
        else
          return std::uint64_t(1);
      };
      // Metal can hold an exact submission, exercising cancellation during
      // initial transfer, closure graph upload and actual device computation.
      const auto count = holds.template operator()<m::Runtime>(submissions);
      for (unsigned submission = 1; submission <= count; ++submission)
        for (auto mode : {Mode::HeldCancel, Mode::HeldDeadline})
          run(argv[1], workers, mode, submission);
      for (auto mode : {Mode::PreCancel, Mode::PreDeadline, Mode::Completion})
        run(argv[1], workers, mode);
      for (unsigned attempt = 1; attempt <= 32; ++attempt)
        run(argv[1], workers, Mode::NativeAllocation, attempt);
      for (unsigned attempt = 1; attempt <= 16; ++attempt)
        run(argv[1], workers, Mode::DeviceAllocation, attempt);
      for (unsigned attempt = 1; attempt <= 8; ++attempt)
        run(argv[1], workers, Mode::Admission, attempt);
    }
    std::cout << "GPU closures: concurrency, cancellation/deadlines, native/device/admission "
                 "failures, fault precedence and zero retained resources passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
