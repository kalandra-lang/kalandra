// Private storage protocol fixture; source-language arrays need typed lowering.
#include "Metal.h"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
namespace {
constexpr std::uint64_t Count = 16385;
constexpr auto Expected = Count * 23 + 17 * Count * (Count - 1) / 2;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, m::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
m::NumericExecutable prepare(m::Runtime &runtime, const char *file) {
  auto device = take(runtime.device(), "physical Metal device");
  std::cout << "Physical device: " << device.name() << '\n';
  const std::array types{NumericType::UInt64};
  const std::array<NumericType, 7> scalars{
      NumericType::UInt64, NumericType::UInt64, NumericType::UInt64, NumericType::UInt64,
      NumericType::UInt64, NumericType::UInt64, NumericType::UInt64};
  const std::array<std::uint32_t, 1> reads{0};
  const std::array<std::uint8_t, 1> roles{0};
  auto kernel = take(runtime.loadNumericGraphKernel(file, device, NumericType::UInt64, {1, 7},
                                                    types, scalars, reads, roles),
                     "load physical arena shader");
  const std::array<std::uint64_t, 7> initial{};
  const m::NumericInstruction op{NumericProgramKind::DeviceControl, kernel, {}, initial, 0};
  auto program =
      take(runtime.prepareNumericProgram(0, std::span(&op, 1), 0, NumericType::UInt64, true),
           "prepare private arena program");
  // The private mutable binding cannot be supplied as a public source input.
  auto bad = op;
  const std::array<std::uint32_t, 1> forged{0};
  bad.inputs = forged;
  check(!runtime.prepareNumericProgram(1, std::span(&bad, 1), 1, NumericType::UInt64, true),
        "forged private arena binding admitted");
  return program;
}
void empty(m::Runtime &runtime, TaskCore &core, std::uint64_t arguments) {
  auto s = runtime.stats();
  auto t = core.stats();
  check(!s.bytes && !s.metalBuffers && !s.inFlight && !s.stagingBuffers &&
            s.argumentBytes == arguments && !t.records && !t.groups && !t.externalPending &&
            !t.retainedBytes,
        "private arena retained resources after settlement");
}
void run(m::Runtime &runtime, const m::NumericExecutable &program, unsigned workers,
         unsigned cancelAt = 0, bool deadline = false, bool deny = false,
         bool allocationProbe = false, std::int64_t expectedFault = 0) {
  auto core = TaskCore::create({.workers = workers});
  check(bool(core), "create arena TaskCore");
  const auto arguments = runtime.stats().argumentBytes;
  const auto start = runtime.stats().submissions;
  if (cancelAt)
    runtime.holdCompletionAt(start + cancelAt);
  auto group = core->openRoot();
  std::array<TaskHandle, 4> handles{};
  const auto count = cancelAt || deny || allocationProbe || expectedFault ? 1u : 4u;
  for (unsigned i = 0; i < count; ++i) {
    handles[i] = runtime.executeNumericProgram(*core, group, program, {});
    check(bool(handles[i]), "submit private arena invocation");
  }
  check(core->close(group), "close arena group");
  if (cancelAt) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (runtime.stats().submissions < start + cancelAt &&
           std::chrono::steady_clock::now() < end) {
      core->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool reached = runtime.stats().submissions == start + cancelAt;
    if (reached)
      check(core->cancel(group, deadline), "cancel held arena page/chunk");
    runtime.holdCompletionAt(0);
    check(reached, "arena never reached held acknowledgement");
  }
  check(core->drain(), "drain private arena tasks");
  for (unsigned i = 0; i < count; ++i) {
    auto failure = core->failure(handles[i]);
    if (cancelAt)
      check(failure.kind == TaskFailure::Cancelled &&
                core->failure(group).kind ==
                    (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
            "arena cancellation lost task/group outcome");
    else if (deny)
      check(failure.kind == TaskFailure::Fault && failure.code == std::int64_t(m::Error::Budget),
            "arena storage denial did not preserve Budget fault");
    else if (expectedFault)
      check(failure.kind == TaskFailure::Fault && failure.code == expectedFault,
            "arena command failure lost native error");
    else if (allocationProbe && failure.kind != TaskFailure::None)
      check(failure.kind == TaskFailure::Fault && (failure.code == 2 || failure.code == 6),
            "arena injected allocation error changed category");
    else {
      Invocation invocation;
      std::uint64_t value = 0;
      check(failure.kind == TaskFailure::None &&
                core->observe(handles[i], *numericScalarLayout(NumericType::UInt64), invocation,
                              &value) &&
                value == Expected,
            "physical arena checksum/concurrent isolation failed");
    }
  }
  if (cancelAt)
    check(runtime.stats().submissions == start + cancelAt, "cancel submitted more arena work");
  check(core->release(group), "release arena task group");
  empty(runtime, *core, arguments);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    m::Options options;
    options.maxBytes = 8 * 1024 * 1024;
    options.maxCachedDeviceBytes = 0;
    options.maxCachedUploadBytes = 0;
    options.cachePreparedOutputs = false;
    auto runtime = take(m::Runtime::create(options), "create physical Metal runtime");
    auto program = prepare(*runtime, argv[1]);
    for (unsigned workers : {1u, 4u}) {
      run(*runtime, program, workers);
      // Held real acknowledgements during initialization and subsequent work.
      for (unsigned at : {1u, 3u, 20u}) {
        run(*runtime, program, workers, at);
        run(*runtime, program, workers, at, true);
      }
    }
    runtime->failNextCompletion();
    run(*runtime, program, 1, 0, false, false, false, std::int64_t(m::Error::Device));
    for (unsigned failure = 1; failure <= 80; ++failure) {
      runtime->failAllocationAt(failure);
      run(*runtime, program, 1, 0, false, false, true);
      runtime->failAllocationAt(0);
    }
    options.maxBytes = 128 * 1024;
    auto limited = take(m::Runtime::create(options), "create budget-limited Metal runtime");
    const std::array<std::uint64_t, 7> zero{};
    // A second arena instruction starts with a fresh frame and private heap.
    // Copying the first instruction's completed PC would silently skip work.
    auto device = take(runtime->device(), "sequence device");
    const std::array types{NumericType::UInt64};
    const std::array<NumericType, 7> frameTypes{
        NumericType::UInt64, NumericType::UInt64, NumericType::UInt64, NumericType::UInt64,
        NumericType::UInt64, NumericType::UInt64, NumericType::UInt64};
    const std::array<std::uint32_t, 1> reads{0};
    const std::array<std::uint8_t, 1> roles{0};
    auto kernel = take(runtime->loadNumericGraphKernel(argv[1], device, NumericType::UInt64, {1, 7},
                                                       types, frameTypes, reads, roles),
                       "sequence kernel");
    const std::array code{
        m::NumericInstruction{NumericProgramKind::DeviceControl, kernel, {}, zero, 0},
        m::NumericInstruction{NumericProgramKind::DeviceControl, kernel, {}, zero, 1}};
    auto sequence = take(runtime->prepareNumericProgram(0, code, 1, NumericType::UInt64, true),
                         "prepare consecutive arena instructions");
    run(*runtime, sequence, 4);
    auto limitedProgram = prepare(*limited, argv[1]);
    run(*limited, limitedProgram, 1, 0, false, true);
    std::cout << "PASS physical Metal private arena: GPU write/read checksum, asynchronous "
                 "growth, concurrency, cancellation, deadlines, denial and cleanup\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
