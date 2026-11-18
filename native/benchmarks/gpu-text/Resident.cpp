// Run the compiler's prefix mask with both text inputs already on the GPU.
// Shader loading, packing, uploads and output verification are outside timing.
#if defined(BENCH_METAL)
#include "Metal.h"
namespace backend = kalandra::runtime::metal;
#elif defined(BENCH_CUDA)
#include "Language.h"
namespace driver = kalandra::runtime::cuda;
namespace backend = driver::language;
#else
#include "Language.h"
namespace driver = kalandra::runtime::vulkan;
namespace backend = driver::language;
#endif
#include "../../runtime/GpuText.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace kalandra;
using namespace kalandra::runtime;

static void require(bool ok) {
  if (!ok)
    throw std::runtime_error("resident GPU text correctness or ownership check failed");
}

template <class T, class E> T take(std::expected<T, E> value) {
  require(bool(value));
  return std::move(*value);
}

static backend::Buffer finish(TaskCore &core, GroupHandle group, TaskHandle task) {
  require(task && core.close(group) && core.drain() &&
          core.failure(task).kind == TaskFailure::None);
  Invocation context;
  alignas(backend::Buffer) std::byte slot[sizeof(backend::Buffer)];
  require(core.observe(task, backend::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<backend::Buffer *>(slot));
  auto result = *value;
  std::destroy_at(value);
  require(core.release(group));
  const auto stats = core.stats();
  require(!stats.records && !stats.retainedBytes && !stats.externalPending);
  return result;
}

int main(int argc, char **argv) {
  try {
    if (argc != 4)
      return 2;
    const auto rows = std::stoull(argv[2]), repetitions = std::stoull(argv[3]);
    require(rows > 0 && rows <= 1048576 && repetitions > 0 && repetitions <= 1000);
    if (std::string_view(argv[1]) == "--native-cpu") {
      // A first-scalar test is a byte prefix test for a valid one-scalar needle.
      constexpr std::array<std::string_view, 4> sample{"سلام 🧪", "", "e\u0301Kalandra", "سیب猫"};
      std::vector<std::string_view> input(rows);
      std::vector<std::uint8_t> mask(rows);
      for (std::size_t i = 0; i < rows; ++i)
        input[i] = sample[i % sample.size()];
      const auto iterations = std::max<std::size_t>(1, 65536 / rows);
      for (std::size_t repeat = 0; repeat <= repetitions; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
          for (std::size_t i = 0; i < rows; ++i)
            mask[i] = input[i].starts_with("س");
          std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        auto end = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < rows; ++i)
          require(mask[i] == (i % 4 == 0 || i % 4 == 3));
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        std::cout << duration / iterations << '\n';
      }
      return 0;
    }
    backend::Options options;
    options.maxBytes = 128 * 1024 * 1024;
#if defined(BENCH_METAL)
    options.cachePreparedOutputs = false;
#endif
    auto runtime = take(backend::Runtime::create(options));
    auto device = take(runtime->device());
    auto core = TaskCore::create({.workers = 1});
    require(bool(core));
    Invocation owner;
    const auto make = [&](std::string_view text) {
      StringDescriptor value;
      kalandra_string_literal_v1(&owner, &value, text.data(), text.size());
      require(!owner.status());
      return value;
    };
    const std::array sample{make("سلام 🧪"), make(""), make("e\u0301Kalandra"), make("سیب猫")};
    auto *input =
        static_cast<StringDescriptor *>(owner.allocateRecord(rows * sizeof(StringDescriptor)));
    require(input);
    for (std::size_t i = 0; i < rows; ++i)
      input[i] = sample[i % sample.size()];
    const auto batch =
        packGpuText(owner, StringArrayDescriptor{input, input, 0, std::int64_t(rows), 1});
    const auto prefix = packGpuText(owner, make("س"));
    require(batch.allocated && prefix.allocated);
    const auto upload = [&](const GpuTextWords &words) {
      auto host = take(runtime->fromCpu(owner, words.view()));
      auto group = core->openRoot();
      return finish(*core, group, runtime->upload(*core, group, host, device));
    };
    const std::array resident{upload(batch), upload(prefix)};
    const std::array tensorTypes{NumericType::UInt32, NumericType::UInt32};
    const std::array scalarTypes{NumericType::Int64};
    WorkgroupLaunch launch{{256}, {1, 1}, {{{WorkgroupShapeAction::Scalar, 0, 0, 0}}}};
    const auto kernel = take(runtime->loadWorkgroupKernel(argv[1], device, NumericType::UInt8,
                                                          tensorTypes, scalarTypes, launch));
    const std::array<std::uint32_t, 2> operands{0, 1};
    const std::array<std::uint64_t, 1> shape{rows};
    backend::NumericInstruction instruction{NumericProgramKind::Workgroup, kernel, operands, shape,
                                            2};
    auto program = take(runtime->prepareNumericProgram(2, std::span(&instruction, 1), 2,
                                                       NumericType::UInt8, false
#if !defined(BENCH_METAL)
                                                       ,
                                                       device
#endif
                                                       ));
    for (std::uint64_t i = 0; i <= repetitions; ++i) {
      auto start = std::chrono::steady_clock::now();
      auto group = core->openRoot();
      auto computed =
          finish(*core, group, runtime->executeNumericProgram(*core, group, program, resident));
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
      auto read = core->openRoot();
      auto output = finish(*core, read, runtime->download(*core, read, computed));
#if defined(BENCH_METAL)
      auto bytes = take(output.hostBytes());
#else
      auto bytes = take(driver::hostBytes(output));
#endif
      require(bytes.size() == rows && ns > 0);
      for (std::size_t j = 0; j < rows; ++j)
        require(bytes[j] == std::byte(j % 4 == 0 || j % 4 == 3));
      std::cout << ns << '\n'; // First observation is the warmup.
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
