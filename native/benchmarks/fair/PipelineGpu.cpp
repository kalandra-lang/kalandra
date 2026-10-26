#include "../../metal/Metal.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef BENCH_FLOAT32
using Float = float;
#else
using Float = double;
#endif
using namespace kalandra;
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
constexpr NumericType Element = sizeof(Float) == 4 ? NumericType::Float32 : NumericType::Float64;
constexpr m::Format Format = sizeof(Float) == 4 ? m::Format::Float32 : m::Format::Float64;
static void checked(bool ok, int line) {
  if (!ok)
    throw std::runtime_error("GPU benchmark operation or cleanup failed at line " +
                             std::to_string(line));
}
#define check(condition) checked(bool(condition), __LINE__)
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
static void retire(TaskCore &core, GroupHandle group) {
  check(core.close(group) && core.drain() && core.release(group));
  const auto stats = core.stats();
  check(!stats.groups && !stats.records && !stats.retainedOwners && !stats.retainedBytes &&
        !stats.externalPending);
}
static m::Buffer upload(m::Runtime &runtime, TaskCore &core, m::Buffer host, m::Device device) {
  const auto group = core.openRoot();
  const auto task = runtime.upload(core, group, host, device);
  check(bool(task) && core.drain());
  auto result = observe(core, task);
  retire(core, group);
  return result;
}
int main(int argc, char **argv) {
  if (argc != 7 && argc != 9)
    return 2;
  const std::string directory = argv[1];
  const unsigned transfers = std::stoul(argv[2]); // 0 resident, 1 all, 2 activations/bias only
  const int kind = std::stoi(argv[3]);
  const std::size_t n = std::stoull(argv[argc == 7 ? 4 : 6]),
                    rows = argc == 7 ? n : std::stoull(argv[4]),
                    inner = argc == 7 ? n : std::stoull(argv[5]),
                    iterations = std::stoull(argv[argc == 7 ? 5 : 7]);
  const int rounds = std::stoi(argv[argc == 7 ? 6 : 8]);
  if (kind != 2 || transfers > 2 || !rows || !inner || !n || !iterations || rounds < 1)
    return 2;
  m::Options options;
  options.profile = std::getenv("KALANDRA_GPU_PROFILE") != nullptr;
  const auto count = rows * n;
  options.maxBytes = std::max(
      options.maxBytes, std::uint64_t(rows * inner + inner * n + count) * sizeof(Float) * 4 + 4096);
  auto made = m::Runtime::create(options);
  check(bool(made));
  auto &runtime = **made;
  auto device = runtime.device();
  check(bool(device));
  TaskLimits limits;
  limits.workers = 1;
  auto core = TaskCore::create(limits);
  check(bool(core));
  const auto stem = directory + "/kernel0";
  auto kernel =
      kind == 2 ? (sizeof(Float) == 4
                       ? runtime.loadMatmulKernel(stem + ".metallib", *device)
                       : runtime.loadNumericMatmulKernel(stem + ".metallib", *device, Element))
      : sizeof(Float) == 4
          ? runtime.loadElementwiseKernel(stem + ".metallib", *device, {2, 0})
          : runtime.loadNumericKernel(stem + ".metallib", *device, Element, {2, 0},
                                      std::array{Element, Element}, std::array<NumericType, 0>{},
                                      std::array<std::uint32_t, 2>{0, 1});
  auto reduce = sizeof(Float) == 4 ? runtime.loadReductionKernel(stem + "-reduce.metallib", *device)
                                   : runtime.loadNumericReductionKernel(stem + "-reduce.metallib",
                                                                        *device, Element);
  check(bool(kernel) && bool(reduce));
  auto post = runtime.loadElementwiseKernel(stem + "-post.metallib", *device, {4, 1},
                                            std::array<std::uint32_t, 2>{0, 3});
  check(bool(post));
  std::vector<Float> a(rows * inner), b(inner * n), bias(count, Float(0.125));
  for (std::size_t i = 0; i < a.size(); ++i)
    a[i] = (int(i % 17) - 8) / 8.0;
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = (int(i % 13) - 6) / 8.0;
  {
    auto ha = runtime.hostTensor(std::as_bytes(std::span(a)),
                                 std::array{std::int64_t(rows), std::int64_t(inner)}, Format);
    auto hb = runtime.hostTensor(std::as_bytes(std::span(b)),
                                 std::array{std::int64_t(inner), std::int64_t(n)}, Format);
    check(bool(ha) && bool(hb));
    auto hc = runtime.hostTensor(std::as_bytes(std::span(bias)),
                                 std::array{std::int64_t(rows), std::int64_t(n)}, Format);
    check(bool(hc));
    m::Buffer residentA, residentB, residentC;
    if (!transfers) {
      residentA = upload(runtime, *core, *ha, *device);
      residentB = upload(runtime, *core, *hb, *device);
      residentC = upload(runtime, *core, *hc, *device);
    } else if (transfers == 2)
      residentB = upload(runtime, *core, *hb, *device);
    const auto baselineBytes = runtime.stats().bytes;
    for (int round = -2; round < rounds; ++round) {
      double checksum = 0;
      const auto before = runtime.stats();
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < iterations; ++i) {
        auto da = transfers ? upload(runtime, *core, *ha, *device) : residentA;
        auto db = transfers == 1 ? upload(runtime, *core, *hb, *device) : residentB;
        auto dc = transfers ? upload(runtime, *core, *hc, *device) : residentC;
        const auto group = core->openRoot();
        const auto task =
            kind == 2
                ? runtime.matmulReduceSum(*core, group, *kernel, *reduce, da, db, {}, {}, {}, {},
                                          *post, std::array{da, da, db, dc}, std::array{0.25f})
            : sizeof(Float) == 4
                ? runtime.elementwiseReduceSum(*core, group, *kernel, *reduce, std::array{da, db},
                                               std::array<float, 0>{})
                : runtime.numericElementwise(*core, group, *kernel, std::array{da, db}, {},
                                             *reduce);
        check(bool(task) && core->drain());
        Invocation context;
        Float value = 0;
        check(core->observe(task, *numericScalarLayout(Element), context, &value));
        checksum += value;
        retire(*core, group);
      }
      const auto ns =
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
              .count();
      const auto stats = runtime.stats();
      check(stats.bytes == baselineBytes && !stats.inFlight && !stats.stagingBuffers &&
            !stats.argumentPacks && !stats.argumentBytes);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"device_id\":%llu", round, ns,
                  checksum, static_cast<unsigned long long>(device->identity()));
      if (options.profile)
        std::printf(",\"gpu_ns\":%llu,\"encoding_ns\":%llu,\"submissions\":%llu,\"dispatches\":%"
                    "llu,\"peak_bytes\":%llu,\"resident_bytes\":%llu",
                    (unsigned long long)(stats.gpuNanoseconds - before.gpuNanoseconds),
                    (unsigned long long)(stats.encodingNanoseconds - before.encodingNanoseconds),
                    (unsigned long long)(stats.submissions - before.submissions),
                    (unsigned long long)(stats.dispatches - before.dispatches),
                    (unsigned long long)stats.peakBytes, (unsigned long long)baselineBytes);
      std::printf("}\n");
    }
    for (std::size_t i = 0; i < a.size(); ++i)
      check(a[i] == (int(i % 17) - 8) / 8.0);
    for (std::size_t i = 0; i < b.size(); ++i)
      check(b[i] == (int(i % 13) - 6) / 8.0);
  }
  const auto stats = runtime.stats();
  check(!stats.bytes && !stats.inFlight && !stats.stagingBuffers && !stats.argumentPacks &&
        !stats.argumentBytes && !stats.hostBackings && !stats.deviceBackings &&
        !stats.metalBuffers);
}
