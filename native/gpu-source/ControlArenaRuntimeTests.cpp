// Infrastructure qualification only: source-language array support additionally
// requires typed allocation/indexing/collection lowering and ownership proofs.
#if defined(KALANDRA_CONTROL_CUDA)
#include "../cuda/Language.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Language.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
constexpr std::uint64_t Count = 16385;
constexpr std::uint64_t Expected = Count * 23 + 17 * Count * (Count - 1) / 2;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
g::Program prepare(g::Runtime &runtime, const char *file) {
  g::ProgramKernel schema;
  schema.type = NumericType::UInt64;
  schema.tensorTypes = {NumericType::UInt64};
  schema.scalarTypes.assign(7, NumericType::UInt64);
  schema.scalarBuffers = {0};
  schema.scalar = true;
  schema.reads = std::vector<std::uint32_t>{0};
  auto contract =
      take(g::kernelContract(schema, NumericProgramKind::DeviceControl), "arena contract");
  schema.kernel = take(runtime.loadKernel(file, &contract), "load actual arena shader");
  g::ProgramInstruction op{NumericProgramKind::DeviceControl, schema, {}, {},
                           std::vector<std::uint64_t>(7),     0};
  return take(g::Program::prepare(runtime, 0, std::array{op}, 0, NumericType::UInt64, true),
              "prepare arena program");
}
std::uint64_t cell(const g::KernelResult &result, std::size_t index) {
  return std::uint64_t(result.parameters.at(1 + index * 2)) |
         (std::uint64_t(result.parameters.at(2 + index * 2)) << 32);
}
void direct(g::Runtime &runtime, const g::Program &program, bool interrupt) {
  const auto baseline = runtime.retainedBytes();
  {
    auto execution = take(program.start(runtime, {}, {}), "start arena invocation");
    bool stopped = false;
    unsigned requests = 0, dispatches = 0;
    for (unsigned step = 0; !execution.finished() && step < 10000; ++step) {
      auto command = take(execution.next(runtime), "advance paged arena");
      if (!command)
        continue;
      ++dispatches;
      auto &c = *command;
      auto result = take(runtime.execute(c.kernel, c.inputs, c.parameters, c.outputBytes, c.groups),
                         "execute GPU arena write/read");
      const bool grow = cell(result, 0) == detail::ControlArenaABI::Grow;
      const auto capacity = cell(result, 1);
      if (grow)
        ++requests;
      auto accepted = execution.accept(runtime, std::move(result));
      check(bool(accepted), "GPU arena result rejected");
      if (interrupt && grow && capacity == 16384) {
        // Old arena is 128KiB: one 64KiB copy must leave both allocations live.
        auto copied = take(execution.next(runtime), "one partial growth page");
        check(!copied && !execution.finished(), "partial growth dispatched source code");
        stopped = true;
        break;
      }
    }
    if (interrupt)
      check(stopped, "probe never reached multi-page growth");
    else {
      check(execution.finished() && requests >= 10 && dispatches > 100,
            "probe did not execute resumable dynamic growth on the GPU");
      auto result = take(execution.result(), "arena scalar result");
      std::uint64_t checksum = 0;
      check(bool(runtime.readBytes(result.data, std::as_writable_bytes(std::span(&checksum, 1)))) &&
                checksum == Expected,
            "GPU checksum lost grown storage contents");
    }
  }
  check(runtime.retainedBytes() == baseline,
        "arena completion/interruption retained device storage");
}
void invalidResults(g::Runtime &runtime, const g::Program &program) {
  const auto baseline = runtime.retainedBytes();
  for (auto flag : {0u, 1u, 2u, 3u, 99u, UINT32_MAX}) {
    auto execution = take(program.start(runtime, {}, {}), "start status probe");
    std::optional<g::ProgramDispatch> command;
    while (!command)
      command = take(execution.next(runtime), "advance status probe");
    auto &c = *command;
    auto result = take(runtime.execute(c.kernel, c.inputs, c.parameters, c.outputBytes, c.groups),
                       "execute status probe");
    result.parameters.back() = flag;
    if (flag == UINT32_MAX)
      ++result.parameters[3]; // Corrupt the capacity rather than a source value.
    auto rejected = execution.accept(runtime, std::move(result));
    const auto expected = flag == 0   ? g::Error::Arithmetic
                          : flag == 1 ? g::Error::Bounds
                          : flag == 2 ? g::Error::Allocation
                          : flag == 3 ? g::Error::Budget
                                      : g::Error::Invalid;
    check(!rejected && rejected.error() == expected, "arena status changed its failure category");
  }
  check(runtime.retainedBytes() == baseline, "status rejection retained arena storage");
}
void copyValidation(g::Runtime &runtime) {
  const auto baseline = runtime.retainedBytes();
  {
    const std::array<std::uint32_t, 4> data{1, 23, 456, 7890};
    auto input = take(runtime.uploadBytes(std::as_bytes(std::span(data))), "copy fixture upload");
    auto output = take(runtime.blankBytes(32), "copy fixture destination");
    check(bool(runtime.copyPrivateBytes(input, output, 4, 12)), "valid private copy range");
    std::array<std::uint32_t, 8> observed{};
    check(bool(runtime.readBytes(output, std::as_writable_bytes(std::span(observed)))) &&
              observed == std::array<std::uint32_t, 8>{0, 23, 456, 7890, 0, 0, 0, 0},
          "private range copy changed bytes outside the requested range");
    check(bool(runtime.copyPrivateBytes(input, output, 0, 12, 4)), "translated copy target offset");
    check(bool(runtime.copyPrivateBytes(input, output, 8, 0, 8)), "translated copy source offset");
    check(bool(runtime.copyPrivateBytes(input, output, input.bytes(), output.bytes(), 0)),
          "zero-byte terminal ranges");
    check(bool(runtime.readBytes(output, std::as_writable_bytes(std::span(observed)))) &&
              observed == std::array<std::uint32_t, 8>{456, 7890, 456, 1, 0, 0, 0, 0},
          "translated copy changed prefix/suffix or copied wrong bytes");
    std::array<std::uint32_t, 4> unchanged{};
    check(bool(runtime.readBytes(input, std::as_writable_bytes(std::span(unchanged)))) &&
              unchanged == data,
          "ranged copy mutated its source");
    for (auto range : {std::array<std::uint64_t, 3>{UINT64_MAX, 0, 4},
                       {0, UINT64_MAX, 4},
                       {12, 0, 8},
                       {0, 28, 8},
                       {1, 0, 4},
                       {0, 1, 4},
                       {0, 0, 3}}) {
      auto rejected = runtime.copyPrivateBytes(input, output, range[0], range[1], range[2]);
      check(!rejected && rejected.error() == g::Error::Invalid, "invalid translated copy admitted");
    }
    auto self = runtime.copyPrivateBytes(output, output, 0, 4, 4);
    check(!self && self.error() == g::Error::Invalid, "same-owner translated copy admitted");
    for (auto range :
         {std::array<std::uint64_t, 2>{UINT64_MAX, 4}, {4, UINT64_MAX}, {1, 4}, {0, 3}, {12, 8}}) {
      auto rejected = runtime.copyPrivateBytes(input, output, range[0], range[1]);
      check(!rejected && rejected.error() == g::Error::Invalid,
            "invalid private copy range admitted");
    }
    auto overlap = runtime.copyPrivateBytes(input, input, 0, 4);
    check(!overlap && overlap.error() == g::Error::Invalid, "overlapping private copy admitted");
    auto immutable =
        take(runtime.uploadBytes(std::as_bytes(std::span(data))), "immutable copy target");
    auto immutableWrite = runtime.copyPrivateBytes(input, immutable, 0, 4);
    check(!immutableWrite && immutableWrite.error() == g::Error::Invalid,
          "private growth overwrote published input storage");
    auto other = take(g::Runtime::create(), "foreign copy owner");
    auto foreign = take(other->blankBytes(16), "foreign private storage");
    auto rejected = runtime.copyPrivateBytes(foreign, output, 0, 4);
    check(!rejected && rejected.error() == g::Error::Residency, "foreign arena storage admitted");
  }
  check(runtime.retainedBytes() == baseline, "copy validation retained backing");
}
void budget(const char *shader) {
  g::Options limits;
  limits.maxBytes = 256 * 1024;
  auto runtime = take(g::Runtime::create(limits), "budget driver");
  auto program = prepare(*runtime, shader);
  bool denied = false;
  {
    auto execution = take(program.start(*runtime, {}, {}), "budget invocation");
    for (unsigned step = 0; step < 10000 && !execution.finished(); ++step) {
      auto command = execution.next(*runtime);
      if (!command) {
        check(command.error() == g::Error::Budget, "arena growth lost budget fault category");
        denied = true;
        break;
      }
      if (!*command)
        continue;
      auto &c = **command;
      auto result = runtime->execute(c.kernel, c.inputs, c.parameters, c.outputBytes, c.groups);
      if (!result) {
        check(result.error() == g::Error::Budget, "arena dispatch lost budget fault category");
        denied = true;
        break;
      }
      check(bool(execution.accept(*runtime, std::move(*result))), "budget probe status");
    }
  }
  check(denied && !runtime->retainedBytes(), "denied arena invocation published/leaked storage");
}
void tasks(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program, bool cancel,
           bool deadline) {
  const auto baseline = runtime.retainedBytes();
  auto core = TaskCore::create({.workers = 4});
  check(bool(core), "create task core");
  auto group = core->openRoot();
  if (cancel)
    async.holdCompletions(true);
  std::array<TaskHandle, 4> handles{};
  const auto count = cancel ? 1u : 4u;
  for (unsigned i = 0; i < count; ++i) {
    handles[i] = async.executeProgram(*core, group, program, {}, {});
    check(bool(handles[i]), "submit arena invocation");
  }
  check(core->close(group), "close arena group");
  if (cancel) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!async.stats().held && std::chrono::steady_clock::now() < limit) {
      core->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto held = async.stats().held;
    if (held)
      check(core->cancel(group, deadline), "cancel initial arena growth");
    async.holdCompletions(false);
    check(held, "arena invocation never reached held growth acknowledgement");
  }
  check(core->drain(), "drain arena task group");
  for (unsigned i = 0; i < count; ++i) {
    if (cancel)
      check(core->failure(handles[i]).kind == TaskFailure::Cancelled &&
                core->failure(group).kind ==
                    (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
            "arena cancellation/deadline outcome");
    else {
      Invocation invocation;
      std::uint64_t checksum = 0;
      check(core->failure(handles[i]).kind == TaskFailure::None &&
                core->observe(handles[i], *numericScalarLayout(NumericType::UInt64), invocation,
                              &checksum) &&
                checksum == Expected,
            "concurrent arena invocations shared mutable state");
    }
  }
  check(core->release(group), "release arena group");
  const auto stats = core->stats();
  const auto driver = async.stats();
  check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes &&
            !driver.pending && !driver.captureBytes && runtime.retainedBytes() == baseline,
        "arena task exit retained resources");
}
// Qualify the shared source-supervisor adapter as well as the raw executor.
// Its Map kernel schema initially includes the private arena as a tensor; the
// DeviceControl instruction must remove that binding from the logical domain.
void language(const char *file) {
  namespace l = g::language;
  l::Options options;
  options.validation = true;
  auto runtime = take(l::Runtime::create(options), "create source adapter");
  auto device = take(runtime->device(), "source adapter device");
  const std::array types{NumericType::UInt64};
  const std::array<NumericType, 7> scalars{
      NumericType::UInt64, NumericType::UInt64, NumericType::UInt64, NumericType::UInt64,
      NumericType::UInt64, NumericType::UInt64, NumericType::UInt64};
  const std::array<std::uint32_t, 1> reads{0};
  const std::array<std::uint8_t, 1> roles{0};
  auto kernel = take(runtime->loadNumericGraphKernel(file, device, NumericType::UInt64, {1, 7},
                                                     types, scalars, reads, roles),
                     "source adapter arena kernel");
  const std::array<std::uint64_t, 7> zero{};
  const std::array code{
      l::NumericInstruction{NumericProgramKind::DeviceControl, kernel, {}, zero, 0},
      l::NumericInstruction{NumericProgramKind::DeviceControl, kernel, {}, zero, 1}};
  auto program = take(runtime->prepareNumericProgram(0, code, 1, NumericType::UInt64, true),
                      "source adapter consecutive arenas");
  const auto baseline = runtime->stats().argumentBytes;
  auto core = TaskCore::create({.workers = 4});
  check(bool(core), "source adapter task core");
  auto group = core->openRoot();
  std::array<TaskHandle, 4> handles;
  for (auto &handle : handles) {
    handle = runtime->executeNumericProgram(*core, group, program, {});
    check(bool(handle), "source adapter arena admission");
  }
  check(core->close(group) && core->drain(), "source adapter arena drain");
  for (auto handle : handles) {
    Invocation context;
    std::uint64_t checksum = 0;
    check(
        core->failure(handle).kind == TaskFailure::None &&
            core->observe(handle, *numericScalarLayout(NumericType::UInt64), context, &checksum) &&
            checksum == Expected,
        "source adapter arena checksum/domain/isolation");
  }
  check(core->release(group), "release source adapter arenas");
  const auto stats = runtime->stats();
  check(!stats.bytes && !stats.inFlight && stats.argumentBytes == baseline,
        "source adapter arena resource retention");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(), "create GPU driver"));
    const auto device = runtime->device();
    check(device.discrete || device.integrated, "arena probe selected a software device");
    std::cout << "Physical device: " << device.name << '\n';
    copyValidation(*runtime);
    if (std::string_view(argv[1]) == "--copy-only") {
#if !defined(KALANDRA_CONTROL_CUDA)
      check(!runtime->validationErrors(), "ranged copy emitted Vulkan validation errors");
#endif
      std::cout << "PASS physical private range copies: distinct offsets, exact bytes, bounds, "
                   "immutable/foreign storage and zero owners\n";
      return 0;
    }
    auto program = prepare(*runtime, argv[1]);
    direct(*runtime, program, false);
    direct(*runtime, program, true);
    invalidResults(*runtime, program);
    auto async = g::AsyncRuntime::create(runtime);
    check(bool(async), "create arena completion service");
    tasks(*runtime, *async, program, false, false);
    tasks(*runtime, *async, program, true, false);
    tasks(*runtime, *async, program, true, true);
    budget(argv[1]);
    language(argv[1]);
#if !defined(KALANDRA_CONTROL_CUDA)
    check(!runtime->validationErrors(), "Vulkan private arena emitted validation errors");
#endif
    std::cout << "PASS physical private arena: device write/read checksum, growth/copy retention, "
                 "concurrent invocations, status faults, cancellation/deadlines and cleanup\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
