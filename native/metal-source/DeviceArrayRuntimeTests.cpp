// Compile against a generated scalar-control fixture. The fixture's numerical
// body remains entirely in its real device shaders; only its CLI is renamed.
#define main device_array_fixture_cli
#include KALANDRA_ARRAY_HOST
#undef main
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
void check(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
void empty(m::Runtime &runtime, TaskCore &core) {
  const auto s = runtime.stats();
  const auto t = core.stats();
  check(!s.bytes && !s.metalBuffers && !s.inFlight && !s.stagingBuffers && !s.argumentBytes &&
            !s.argumentPacks && !t.records && !t.groups && !t.externalPending && !t.retainedBytes,
        "checkpoint invocation retained resources");
}
void execute(m::Runtime &runtime, const std::string &directory, unsigned workers, bool cancel,
             bool expectFault, bool allocationProbe = false, bool deadlineExceeded = false,
             unsigned checkpoint = 3) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  check(bool(core), "create task core");
  const auto start = runtime.stats().submissions;
  if (cancel)
    runtime.holdCompletionAt(
        start +
        checkpoint); // A held real source-control acknowledgement, after heap initialization.
  const auto group = core->openRoot();
  PacketData packet{&runtime, directory.c_str()};
  const auto handle = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
  check(bool(handle) && core->close(group), "submit generated control entry");
  if (cancel) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (runtime.stats().submissions < start + checkpoint &&
           std::chrono::steady_clock::now() < deadline) {
      core->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto reached = runtime.stats().submissions == start + checkpoint;
    if (reached)
      check(core->cancel(group, deadlineExceeded), "cancel at submitted control checkpoint");
    runtime.holdCompletionAt(0); // Always release before draining, including failed assertions.
    check(reached, "device array fixture did not reach acknowledgement " +
                       std::to_string(checkpoint) +
                       "; submitted=" + std::to_string(runtime.stats().submissions - start));
  }
  check(core->drain(), "drain checkpointed invocation");
  const auto failure = core->failure(handle);
  if (expectFault)
    check(failure.kind == TaskFailure::Fault && failure.code == 5,
          "device checked fault was not propagated");
  else if (cancel) {
    check(failure.kind == TaskFailure::Cancelled, "loop cancellation did not settle");
    check(core->failure(group).kind ==
              (deadlineExceeded ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
          "loop cancellation lost its deadline/group outcome");
    check(runtime.stats().submissions == start + checkpoint,
          "cancellation submitted another control chunk");
  } else if (!allocationProbe || failure.kind == TaskFailure::None) {
    check(failure.kind == TaskFailure::None, "scalar control failed");
    Invocation context;
    NumericValue<kalandra::NumericType::Int64> value{};
    check(core->observe(handle, scalarLayout, context, &value) && value == 500500,
          "checkpointed result differs from independent sum oracle");
    check(runtime.stats().submissions > start + 20,
          "array fixture did not resume/collect across chunks");
  }
  if (allocationProbe && failure.kind != TaskFailure::None)
    check(failure.kind == TaskFailure::Fault &&
              (failure.code == 2 || failure.code == std::int64_t(m::Error::Budget)),
          "allocation failure was misreported as another execution error");
  if (expectFault) {
    core->cancel(group);
    check(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 5,
          "later cancellation replaced an already recorded checked fault");
  }
  check(core->release(group), "release checkpoint group");
  empty(runtime, *core);
}
void concurrent(m::Runtime &runtime, const std::string &directory) {
  TaskLimits limits;
  limits.workers = 4;
  auto core = TaskCore::create(limits);
  check(bool(core), "create concurrent core");
  const auto group = core->openRoot();
  PacketData packet{&runtime, directory.c_str()};
  std::array<TaskHandle, 4> handles;
  for (auto &handle : handles) {
    handle = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    check(bool(handle), "concurrent control admission");
  }
  check(core->close(group) && core->drain(), "concurrent checkpoint settlement");
  for (const auto handle : handles) {
    Invocation context;
    NumericValue<kalandra::NumericType::Int64> value{};
    check(core->failure(handle).kind == TaskFailure::None &&
              core->observe(handle, scalarLayout, context, &value) && value == 500500,
          "concurrent invocations shared device control state");
  }
  check(core->release(group), "release concurrent invocations");
  empty(runtime, *core);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  try {
    m::Options options;
    options.profile = true;
    // 1,000 fresh two-element arrays need >48 KiB without reclamation.
    // Four concurrent invocations still fit this bound with a live alias each.
    options.maxBytes = 16 * 1024;
    options.maxCachedDeviceBytes = 0;
    options.maxCachedUploadBytes = 0;
    options.cachePreparedOutputs = false;
    options.maxArgumentBytes = 64 * 1024;
    auto made = m::Runtime::create(options);
    check(bool(made), "create Metal runtime");
    auto &runtime = **made;
    const std::string directory = argv[1];
    if (argc == 3) {
      const bool fault = std::string_view(argv[2]) == "fault";
      for (auto workers : {1u, 4u}) {
        if (fault)
          execute(runtime, directory, workers, false, true);
        // Existing task semantics: cancellation before an external result is
        // acknowledged wins. An unobserved device flag is not a recorded fault.
        execute(runtime, directory, workers, true, false);
        execute(runtime, directory, workers, true, false, false, true);
        if (!fault) {
          execute(runtime, directory, workers, true, false, false, false, 100);
          execute(runtime, directory, workers, true, false, false, true, 100);
        }
      }
      std::cout << "PASS: "
                << (fault ? "checked faults and cancellation before acknowledgement"
                          : "infinite loop cancellation")
                << '\n';
      return 0;
    }
    for (auto workers : {1u, 4u}) {
      for (unsigned i = 0; i < 12; ++i)
        execute(runtime, directory, workers, false, false);
      execute(runtime, directory, workers, true, false);
      execute(runtime, directory, workers, true, false, false, true);
      execute(runtime, directory, workers, true, false, false, false, 100);
      execute(runtime, directory, workers, true, false, false, true, 100);
    }
    concurrent(runtime, directory);
    for (unsigned failure = 1; failure <= 64; ++failure) {
      runtime.failAllocationAt(failure);
      execute(runtime, directory, 1, false, false, true);
      runtime.failAllocationAt(0);
    }
    std::cout << "PASS: bounded device-array reclamation, retained aliases, concurrent heaps, "
                 "cancellation, allocation failures and zero "
                 "retained resources\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
