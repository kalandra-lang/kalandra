// Actual admitted array jobs over source-emitted shaders. Raw native inputs
// exercise NaN payloads and offsets without floating arithmetic in the harness.
#if defined(KALANDRA_ARRAY_METAL)
#include "../metal/Metal.h"
namespace g = kalandra::runtime::metal;
#elif defined(KALANDRA_ARRAY_CUDA)
#include "../cuda/Language.h"
namespace g = kalandra::runtime::cuda::language;
#else
#include "../vulkan/Language.h"
namespace g = kalandra::runtime::vulkan::language;
#endif
#include "../runtime/DeviceArrayInputJob.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
struct Proxy {
  g::Runtime &runtime;
  std::atomic<unsigned> resumes{0}, afterAssembly{0}, pages{0};
  std::atomic<std::uint64_t> assembled{0};
  std::atomic<bool> metadata{false}, pageHeld{false};
  std::uint64_t count = 0;
  unsigned holdPage = 0;
  inline static Proxy *active = nullptr; // Stable for a settled test group.
  auto fromCpu(const Invocation &context, CpuTensorView view) {
    return runtime.fromCpu(context, view);
  }
  auto upload(Task &task, GroupHandle group, g::Buffer buffer, g::Device device) {
    return runtime.upload(task, group, std::move(buffer), device);
  }
  auto executeNumericProgram(Task &task, GroupHandle group, const g::NumericExecutable &program,
                             std::span<const g::Buffer> values,
                             std::span<const std::uint64_t> scalars) {
    return runtime.executeNumericProgram(task, group, program, values, scalars);
  }
  auto downloadArrayPage(Task &task, GroupHandle group, const g::Buffer &buffer,
                         std::uint64_t offset, std::uint64_t words) {
    if (++pages == holdPage) {
      runtime.holdCompletions(true);
      pageHeld = true;
    }
    return runtime.downloadArrayPage(task, group, buffer, offset, words);
  }
  static const ValueLayout &bufferLayout() { return g::Runtime::bufferLayout(); }
  static auto arrayResult(const g::Buffer &value) {
    active->metadata = true;
    return g::Runtime::arrayResult(value);
  }
  static auto arrayPageBytes(const g::Buffer &value) {
    auto bytes = g::Runtime::arrayPageBytes(value);
    if (bytes)
      active->assembled += bytes->size() / 8;
    return bytes;
  }
};
g::NumericExecutable prepare(g::Runtime &runtime, const std::filesystem::path &directory) {
  unsigned slots = 0;
  std::ifstream(directory / "slots") >> slots;
  check(slots >= DeviceArrayExportABI::Cells + 1, "missing result shader schema");
  const auto device = take(runtime.device(), "select device");
  const std::array types{NumericType::UInt64, NumericType::UInt64};
  const std::array<std::uint32_t, 2> reads{0, 1};
  const std::array<std::uint8_t, 2> roles{2, 3};
  const std::vector<NumericType> scalarTypes(slots, NumericType::UInt64);
#if defined(KALANDRA_ARRAY_METAL)
  const auto path = directory / "kernel0.metallib";
#elif defined(KALANDRA_ARRAY_CUDA)
  const auto path = directory / "kernel0.ptx";
#else
  const auto path = directory / "kernel0.spv";
#endif
  const auto native = path.string();
  auto kernel = take(runtime.loadNumericGraphKernel(native, device, NumericType::UInt64, {2, slots},
                                                    types, scalarTypes, reads, roles),
                     "load result kernel");
  const std::array<std::uint32_t, 1> inputs{0};
  const std::vector<std::uint64_t> constants(slots);
  const std::array code{
      g::NumericInstruction{NumericProgramKind::DeviceControl, kernel, inputs, constants, 1}};
#if defined(KALANDRA_ARRAY_METAL)
  return take(runtime.prepareNumericProgram(1, code, 1, NumericType::UInt64, false),
              "prepare result program");
#else
  return take(runtime.prepareNumericProgram(1, code, 1, NumericType::UInt64, false, device),
              "prepare result program");
#endif
}
void empty(g::Runtime &runtime, TaskCore &core, std::uint64_t arguments) {
  const auto s = runtime.stats();
  const auto t = core.stats();
  check(!s.bytes && !s.inFlight && s.argumentBytes == arguments && !t.records && !t.groups &&
            !t.externalPending && !t.retainedBytes && !t.recursiveBytes,
        "settled result job retained resource owners");
#if defined(KALANDRA_ARRAY_METAL)
  check(!s.deviceBackings && !s.hostBackings && !s.metalBuffers && !s.stagingBuffers,
        "settled result job retained Metal backing");
#endif
}
enum class Stop { None, Pack, Metadata, Page, Decode };
template <NumericType Element, bool Boolean = false>
void run(g::Runtime &runtime, const g::NumericExecutable &program, unsigned count, bool alias,
         unsigned workers = 1, unsigned concurrent = 1, Stop stop = Stop::None,
         unsigned checkpoint = 0, bool deadline = false, std::size_t taskBytes = 1024 * 1024) {
  using Cell = NumericValue<Element>;
  using Descriptor = CpuTensor<Element, 1>;
  using Job =
      DeviceArrayResultJob<Proxy, g::Buffer, g::Device, g::NumericExecutable, 0x4b414c41524a0001,
                           Element, Boolean, 0, DeviceArrayArgument<Element, Boolean>>;
  Invocation source;
  auto *data = static_cast<Cell *>(source.allocate((count + 2) * sizeof(Cell)));
  check(bool(data), "native source allocation");
  for (std::uint64_t i = 0; i < count + 2; ++i) {
    std::uint64_t bits = Boolean ? i % 2 : i * UINT64_C(0x9e3779b97f4a7c15);
    if constexpr (Element == NumericType::Float32) {
      if (i % 257 == 0)
        bits = UINT64_C(0x80000000);
      if (i % 257 == 1)
        bits = UINT64_C(0x7fc00000) | (i & UINT64_C(0x003fffff));
      if (i % 257 == 2)
        bits = 1;
    } else if constexpr (Element == NumericType::Float64) {
      if (i % 257 == 0)
        bits = UINT64_C(0x8000000000000000);
      if (i % 257 == 1)
        bits = UINT64_C(0x7ff8000000000000) | i;
      if (i % 257 == 2)
        bits = 1;
    }
    std::memcpy(data + i, &bits, sizeof(Cell));
  }
  Descriptor input{data, data, Boolean ? 0 : 1, {count}, {1}};
  if constexpr (Boolean)
    if (!count)
      input = {}; // Bool native ABI has canonical owned, zero-offset storage.
  Proxy proxy{runtime};
  proxy.count = count;
  proxy.holdPage = stop == Stop::Page ? checkpoint : 0;
  Proxy::active = &proxy;
  TaskLimits limits;
  limits.workers = workers;
  limits.bytes = taskBytes;
  auto core = TaskCore::create(limits);
  check(bool(core), "create result job core");
  const auto arguments = runtime.stats().argumentBytes;
  const auto group = core->openRoot();
  typename Job::PacketData packet{
      &proxy, program, take(runtime.device(), "job device"), {}, std::tuple{input}};
  auto descriptor = Job::entry;
  descriptor.resume = [](Task &task) {
    ++Proxy::active->resumes;
    if (Proxy::active->assembled == Proxy::active->count)
      ++Proxy::active->afterAssembly;
    Job::entry.resume(task);
  };
  std::vector<TaskHandle> handles;
  for (unsigned i = 0; i < concurrent; ++i) {
    auto task = core->submit(group, descriptor,
                             {descriptor.packetLayout, &packet, sizeof(packet), &source});
    check(bool(task), "admit native result job");
    handles.push_back(task);
  }
  check(core->close(group), "close result job group");
  if (stop != Stop::None) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool reached = false;
    while (std::chrono::steady_clock::now() < end) {
      core->step();
      reached = stop == Stop::Pack       ? proxy.resumes >= checkpoint
                : stop == Stop::Metadata ? proxy.metadata.load()
                : stop == Stop::Decode
                    ? proxy.afterAssembly >= checkpoint
                    : proxy.pageHeld && core->stats().externalPending && runtime.stats().inFlight;
      if (reached || core->failure(handles[0]).kind != TaskFailure::None)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (reached)
      core->cancel(group, deadline);
    runtime.holdCompletions(
        false); // Release actual acknowledgement before any assertion/destruction.
    check(reached, "result job never reached selected cancellation phase");
  }
  check(core->drain(), "settle native result jobs");
  {
    Invocation observed;
    std::vector<Descriptor> outputs;
    for (const auto task : handles) {
      const auto failure = core->failure(task);
      if (stop != Stop::None) {
        check(failure.kind == TaskFailure::Cancelled &&
                  core->failure(group).kind ==
                      (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "result job cancellation/deadline changed outcome");
      } else {
        if (failure.kind != TaskFailure::None)
          throw std::runtime_error(
              "native result job faulted: element=" + std::to_string(unsigned(Element)) +
              " Bool=" + std::to_string(Boolean) + " count=" + std::to_string(count) +
              " alias=" + std::to_string(alias) + " workers=" + std::to_string(workers) +
              " failure=" + std::to_string(unsigned(failure.kind)) + ":" +
              std::to_string(failure.code));
        Descriptor result{};
        check(core->observe(task, hostArrayLayout<Element, Boolean>(), observed, &result),
              "observe native array result");
        check(result.sizes[0] == count && (!count || result.strides[0] == 1),
              "native result shape differs");
        if (alias)
          check(result.allocated == input.allocated && result.aligned == input.aligned &&
                    result.offset == input.offset,
                "captured result lost original native alias or offset");
        else if (count)
          check(result.allocated != input.allocated && result.offset == 0,
                "fresh result reused input owner");
        if (count)
          check(!std::memcmp(result.aligned + result.offset, data + (Boolean ? 0 : 1),
                             count * sizeof(Cell)),
                "result changed raw element bits, order, NaN payloads or signed zeros");
        if (!alias && count)
          for (const auto &other : outputs)
            check(other.allocated != result.allocated,
                  "concurrent jobs shared fresh native result backing");
        outputs.push_back(result);
      }
    }
  }
  check(core->release(group), "release native result jobs");
  empty(runtime, *core, arguments);
  source.release(data);
  check(!source.live() && source.allocations() == source.releases(),
        "original/result native owners leaked");
  Proxy::active = nullptr;
}
template <NumericType Element, bool Boolean = false>
void type(g::Runtime &runtime, const std::filesystem::path &root, const char *name) {
  auto alias = prepare(runtime, root / (std::string("alias-") + name));
  auto fresh = prepare(runtime, root / (std::string("fresh-") + name));
  for (unsigned count : {0u, 3u, 17000u}) {
    run<Element, Boolean>(runtime, alias, count, true);
    run<Element, Boolean>(runtime, fresh, count, false);
  }
  run<Element, Boolean>(runtime, fresh, 17000, false, 4, 4, Stop::None, 0, false, 8 * 1024 * 1024);
  std::cout
      << name
      << ": raw bits, offsets, empty/multipage, aliases and concurrent native results passed\n"
      << std::flush;
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    g::Options options;
#if defined(KALANDRA_ARRAY_METAL)
    options.maxCachedDeviceBytes = options.maxCachedUploadBytes = 0;
    options.cachePreparedOutputs = false;
#endif
    auto runtime = take(g::Runtime::create(options), "create physical runtime");
    const std::filesystem::path root(argv[1]);
    type<NumericType::Int8>(*runtime, root, "i8");
    type<NumericType::Int16>(*runtime, root, "i16");
    type<NumericType::Int32>(*runtime, root, "i32");
    type<NumericType::Int64>(*runtime, root, "i64");
    type<NumericType::UInt8>(*runtime, root, "u8");
    type<NumericType::UInt16>(*runtime, root, "u16");
    type<NumericType::UInt32>(*runtime, root, "u32");
    type<NumericType::UInt64>(*runtime, root, "u64");
    type<NumericType::Float32>(*runtime, root, "f32");
    type<NumericType::Float64>(*runtime, root, "f64");
    type<NumericType::UInt8, true>(*runtime, root, "bool");
    auto program = prepare(*runtime, root / "fresh-i64");
    for (bool deadline : {false, true}) {
      for (unsigned step : {1u, 2u})
        run<NumericType::Int64>(*runtime, program, 17000, false, 1, 1, Stop::Pack, step, deadline);
      run<NumericType::Int64>(*runtime, program, 17000, false, 1, 1, Stop::Metadata, 0, deadline);
      for (unsigned page : {1u, 2u, 3u})
        run<NumericType::Int64>(*runtime, program, 17000, false, 1, 1, Stop::Page, page, deadline);
      for (unsigned resume : {1u, 2u, 3u, 4u})
        run<NumericType::Int64>(*runtime, program, 17000, false, 1, 1, Stop::Decode, resume,
                                deadline);
    }
    std::cout
        << "PASS admitted result jobs: all eleven raw types, native alias identity, bounded pages, "
           "cancel/deadline during packing, result assembly, held downloads and decoding; zero "
           "owners\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
