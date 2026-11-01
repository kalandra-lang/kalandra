#include "Language.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::vulkan::language;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, m::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %d\n", message, int(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte storage[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), context, storage), "observe language value");
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(storage));
  auto output = *value;
  std::destroy_at(value);
  return output;
}
struct Hold {
  m::Runtime &runtime;
  explicit Hold(m::Runtime &r) : runtime(r) { runtime.holdCompletions(true); }
  ~Hold() { runtime.holdCompletions(false); }
};
int run(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  const bool validation = argc == 2 || std::string_view(argv[2]) != "--no-validation";
  auto runtime = take(m::Runtime::create({.validation = validation, .maxInFlight = 1}), "registry");
  check(runtime->stats().initializedDevices == 1, "only default context eagerly created");
  auto missing = runtime->device(runtime->deviceCount());
  check(!missing && missing.error() == m::Error::Unavailable &&
            runtime->stats().initializedDevices == 1,
        "invalid selection does not create a context");
  {
    Hold hold(*runtime);
    std::atomic<bool> valid{true};
    std::array<std::thread, 8> selectors;
    const auto last = runtime->deviceCount() - 1;
    for (auto &selector : selectors)
      selector = std::thread([&] {
        auto selected = runtime->device(last);
        if (!selected || !*selected || selected->name().empty())
          valid = false;
        const auto count = runtime->stats().initializedDevices;
        if (count != (last ? 2u : 1u))
          valid = false;
      });
    for (auto &selector : selectors)
      selector.join();
    check(valid && runtime->stats().initializedDevices == (last ? 2u : 1u),
          "concurrent selection publishes exactly one additional context");
  }
  auto core = TaskCore::create({.workers = 4});
  check(bool(core), "core");
  Invocation producer;
  std::array<float, 3> data{1, 2, 3};
  auto cpu = copyCpuTensor<NumericType::Float32, 1>(producer, NumericType::Float32,
                                                    std::as_bytes(std::span(data)),
                                                    std::array<std::int64_t, 1>{3});
  check(bool(cpu), "CPU descriptor");
  auto host = take(runtime->fromCpu(producer, cpu->view()), "host lease");
  producer.settle();
  for (unsigned index = 0; index < runtime->deviceCount(); ++index) {
    auto device = take(runtime->device(index), "device");
    check(bool(device) && !device.name().empty(), "physical device identity");
    auto uploadGroup = core->openRoot();
    auto uploaded = runtime->upload(*core, uploadGroup, host, device);
    check(bool(uploaded) && core->close(uploadGroup) && core->drain(), "upload");
    auto input = observe(*core, uploaded);
    check(core->release(uploadGroup), "upload release");
    auto kernel = take(runtime->loadNumericGraphKernel(
                           std::filesystem::path(argv[1]).string() + "/float32/kernel1.spv", device,
                           NumericType::Float32, {1, 0}, std::array{NumericType::Float32}, {},
                           std::array{0u}, std::array<std::uint8_t, 1>{0}),
                       "kernel");
    std::array<unsigned, 1> ids{0};
    std::array<m::NumericInstruction, 1> code{
        {{m::NumericInstructionKind::Map, kernel, ids, {}, 1}}};
    auto plan =
        take(runtime->prepareNumericProgram(1, code, 1, NumericType::Float32, false, device),
             "prepared plan");
    check(plan.on(device) && runtime->stats().argumentBytes, "plan owns metadata quota");
    for (unsigned other = 0; other < runtime->deviceCount(); ++other)
      if (other != index)
        check(!plan.on(take(runtime->device(other), "other device")),
              "plan retains selected device");
    auto computeGroup = core->openRoot();
    auto computed = runtime->executeNumericProgram(*core, computeGroup, plan, std::array{input});
    input = {};
    plan = {};
    kernel = {};
    code = {};
    check(bool(computed) && core->close(computeGroup) && core->drain(),
          "compute after plan aliases dropped");
    auto output = observe(*core, computed);
    check(core->release(computeGroup), "compute release");
    auto downloadGroup = core->openRoot();
    auto downloaded = runtime->download(*core, downloadGroup, output);
    output = {};
    check(bool(downloaded) && core->close(downloadGroup) && core->drain(), "download");
    auto answer = observe(*core, downloaded);
    check(core->release(downloadGroup), "download release");
    Invocation consumer;
    auto descriptor =
        take(runtime->toCpuTyped<NumericType::Float32, 1>(consumer, answer), "CPU result");
    for (unsigned i = 0; i < 3; ++i)
      check(descriptor.aligned[i] == data[i] + 3, "source adapter independent result");
    check(!runtime->stats().argumentBytes && !runtime->stats().inFlight,
          "plan/capture ledgers retired");
  }
  {
    Hold hold(*runtime);
    auto group = core->openRoot();
    auto first = runtime->upload(*core, group, host, take(runtime->device(), "default"));
    check(bool(first) && core->close(group), "held registry upload");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!core->stats().externalPending && std::chrono::steady_clock::now() < end) {
      core->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(runtime->stats().inFlight == 1 && core->stats().externalPending == 1,
          "global admission held");
    auto rejectedGroup = core->openRoot();
    auto rejected =
        runtime->upload(*core, rejectedGroup, host,
                        take(runtime->device(runtime->deviceCount() - 1), "last device"));
    check(bool(rejected) && core->close(rejectedGroup),
          "global limit rejection returns fault task");
    runtime->holdCompletions(false);
    check(core->drain() && core->failure(first).kind == TaskFailure::None &&
              core->failure(rejected).kind == TaskFailure::Fault &&
              core->failure(rejected).code == 6,
          "shared admission across device services");
    check(core->release(group) && core->release(rejectedGroup), "held group release");
  }
  {
    auto foreign = take(m::Runtime::create({.validation = validation}), "foreign registry");
    Invocation consumer;
    auto invalid = foreign->toCpuTyped<NumericType::Float32, 1>(consumer, host);
    check(!invalid && invalid.error() == m::Error::Residency,
          "CPU export rejects foreign registry backing");
    auto group = core->openRoot();
    auto rejected = foreign->upload(*core, group, host, take(foreign->device(), "foreign target"));
    check(bool(rejected) && core->close(group) && core->drain() &&
              core->failure(rejected).kind == TaskFailure::Fault &&
              core->failure(rejected).code == 7 && core->release(group),
          "upload rejects foreign registry backing");
  }
  host = {};
  check(!runtime->stats().bytes && !runtime->stats().argumentBytes && !runtime->stats().inFlight &&
            !core->stats().records,
        "registry cleanup");
  std::puts("PASS: source runtime adapter, selected devices, prepared ownership and registry-wide "
            "admission");
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
