#include "Metal.h"
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, m::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), context, slot), "observe buffer");
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = std::move(*value);
  value->~Buffer();
  return result;
}
m::NumericExecutable prepare(m::Runtime &runtime, const char *file, unsigned slots) {
  const std::array types{NumericType::UInt64, NumericType::UInt64};
  const std::array<std::uint32_t, 2> reads{0, 1};
  const std::array<std::uint8_t, 2> roles{2, 3};
  std::vector<NumericType> scalarTypes(slots, NumericType::UInt64);
  auto kernel = take(runtime.loadNumericGraphKernel(file, *runtime.device(), NumericType::UInt64,
                                                    {2, slots}, types, scalarTypes, reads, roles),
                     "load export shader");
  std::array<std::uint32_t, 1> inputs{0};
  std::vector<std::uint64_t> constants(slots);
  m::NumericInstruction op{NumericProgramKind::DeviceControl, kernel, inputs, constants, 1};
  check(!runtime.prepareNumericProgram(1, std::array{op}, 1, NumericType::UInt64, true),
        "export admitted scalar result");
  return take(runtime.prepareNumericProgram(1, std::array{op}, 1, NumericType::UInt64, false),
              "prepare terminal export");
}
std::uint64_t run(m::Runtime &runtime, const m::NumericExecutable &program, unsigned n,
                  bool alias) {
  const auto baseline = runtime.stats();
  std::uint64_t submissions = 0;
  {
    std::vector<std::uint64_t> wire{DeviceArrayWireABI::Magic, 4, n, 1};
    for (unsigned i = 0; i < n; ++i)
      wire.push_back(i * 17 + 9);
    const std::int64_t size = wire.size();
    auto host = take(
        runtime.hostTensor(std::as_bytes(std::span(wire)), std::span(&size, 1), m::Format::UInt64),
        "host wire");
    auto core = TaskCore::create();
    check(bool(core), "create task core");
    auto group = core->openRoot();
    auto upload = runtime.upload(*core, group, host, *runtime.device());
    check(upload && core->drain(), "settle upload");
    auto input = observe(*core, upload);
    const auto start = runtime.stats().submissions;
    auto task = runtime.executeNumericProgram(*core, group, program, std::array{input});
    check(task && core->drain(), "settle array computation");
    check(core->failure(task).kind == TaskFailure::None, "array export task fault");
    submissions = runtime.stats().submissions - start;
    auto result = observe(*core, task);
    check(result.arrayResult() == DeviceArrayResultMetadata{4, n, alias ? 1u : 0u},
          "result metadata differs");
    const auto count = alias ? 0 : n;
    check(result.format() == m::Format::UInt64 && result.bytes() == count * 8 &&
              result.shape().size() == 1 && result.shape()[0] == count,
          "array payload has logical array shape");
    auto download = runtime.download(*core, group, result);
    check(download && core->drain(), "settle payload download");
    auto output = observe(*core, download);
    check(output.arrayResult() == result.arrayResult(), "download erased private array identity");
    auto bytes = take(output.hostBytes(), "host payload");
    for (unsigned i = 0; i < count; ++i) {
      std::uint64_t value;
      std::memcpy(&value, bytes.data() + i * 8, 8);
      check(value == wire[4 + i], "export changed value bits");
    }
    Invocation context;
    check(!runtime.toCpuTyped<NumericType::UInt64, 1>(context, output),
          "array envelope accepted as numerical tensor");
    check(core->close(group) && core->drain() && core->release(group), "release export tasks");
    auto stats = core->stats();
    check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes,
          "task result retained owners");
  }
  const auto after = runtime.stats();
  check(after.bytes == baseline.bytes && after.hostBackings == baseline.hostBackings &&
            after.deviceBackings == baseline.deviceBackings && !after.inFlight &&
            after.argumentBytes == baseline.argumentBytes &&
            after.argumentPacks == baseline.argumentPacks,
        "export retained arena/payload owners");
  return submissions;
}
void cancelExport(m::Runtime &runtime, const m::NumericExecutable &program,
                  std::uint64_t submission, bool deadline) {
  const auto baseline = runtime.stats();
  {
    std::vector<std::uint64_t> wire{DeviceArrayWireABI::Magic, 4, 17000, 1};
    wire.resize(17004, 17);
    const std::int64_t size = wire.size();
    auto host = take(
        runtime.hostTensor(std::as_bytes(std::span(wire)), std::span(&size, 1), m::Format::UInt64),
        "cancel host wire");
    auto core = TaskCore::create();
    check(bool(core), "cancel core");
    auto group = core->openRoot();
    auto upload = runtime.upload(*core, group, host, *runtime.device());
    check(upload && core->drain(), "cancel upload");
    auto input = observe(*core, upload);
    const auto start = runtime.stats().submissions;
    runtime.holdCompletionAt(start + submission);
    auto task = runtime.executeNumericProgram(*core, group, program, std::array{input});
    check(bool(task), "submit cancelled export");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (runtime.stats().submissions < start + submission &&
           std::chrono::steady_clock::now() < end) {
      core->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool reached = runtime.stats().submissions == start + submission;
    bool cancelled = false;
    if (reached)
      cancelled = core->cancel(group, deadline);
    runtime.holdCompletionAt(0);
    check(reached && cancelled, "export did not reach held cancellation page");
    check(core->close(group) && core->drain(), "settle cancelled export");
    check(core->failure(task).kind == TaskFailure::Cancelled &&
              core->failure(group).kind ==
                  (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
          "export cancellation/deadline outcome changed");
    check(runtime.stats().submissions == start + submission,
          "cancel submitted another export page");
    check(core->release(group), "release cancelled export");
    auto stats = core->stats();
    check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes,
          "cancelled export retained task owners");
  }
  const auto after = runtime.stats();
  check(after.bytes == baseline.bytes && after.deviceBackings == baseline.deviceBackings &&
            after.hostBackings == baseline.hostBackings && !after.inFlight &&
            after.argumentBytes == baseline.argumentBytes &&
            after.argumentPacks == baseline.argumentPacks,
        "cancelled export retired owners incorrectly");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    m::Options options;
    options.maxCachedUploadBytes = options.maxCachedDeviceBytes = 0;
    auto runtime = take(m::Runtime::create(options), "create Metal runtime");
    const std::filesystem::path root(argv[1]);
    unsigned aliasSlots = 0, freshSlots = 0;
    std::ifstream(root / "alias/slots") >> aliasSlots;
    std::ifstream(root / "fresh/slots") >> freshSlots;
    check(aliasSlots && freshSlots, "missing export fixture metadata");
    auto alias = prepare(*runtime, (root / "alias/kernel0.metallib").c_str(), aliasSlots);
    auto fresh = prepare(*runtime, (root / "fresh/kernel0.metallib").c_str(), freshSlots);
    std::uint64_t freshSubmissions = 0;
    for (auto n : {0u, 1u, 3u, 17000u}) {
      run(*runtime, alias, n, true);
      freshSubmissions = run(*runtime, fresh, n, false);
    }
    check(freshSubmissions >= 3, "missing multipage export submissions");
    for (unsigned page = 0; page < 3; ++page)
      for (bool deadline : {false, true})
        cancelExport(*runtime, fresh, freshSubmissions - 2 + page, deadline);
    std::cout << "PASS Metal: terminal fresh/alias array results, empty/multipage bytes, "
                 "payload identity, tensor isolation, cancellation/deadlines at all export pages "
                 "and zero owners\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
