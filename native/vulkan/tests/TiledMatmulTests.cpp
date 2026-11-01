#include "Language.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <new>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, static_cast<long long>(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
v::Program program(v::Runtime &runtime, const char *path, bool tiled) {
  v::ProgramKernel schema;
  schema.kernel = take(runtime.loadMatmulKernel(path), "load matrix module");
  check(schema.kernel.tiledMatmul() == tiled, "actual matrix algorithm selected");
  if (tiled)
    schema.matmulTranspose = take(runtime.loadBuiltinTransposeFloat32(), "embedded layout module");
  v::ProgramInstruction op{.kind = NumericProgramKind::Matmul, .kernel = schema,
      .inputs = {0, 1}, .scalarSources = {}, .constants = {}, .output = 2, .target = 0};
  const std::array code{op};
  return take(v::Program::prepare(runtime, 2, code, 2, NumericType::Float32, false), "prepare matrix program");
}
v::TensorValue upload(v::Runtime &runtime, std::span<const float> data, unsigned rows, unsigned columns) {
  return {take(runtime.upload(data), "matrix upload"), NumericType::Float32, {rows, columns}, 2};
}
void verify(v::Runtime &runtime, const v::TensorValue &result, const std::vector<float> &a,
            const std::vector<float> &b, unsigned m, unsigned n, unsigned k) {
  check(result.rank == 2 && result.dimensions[0] == m && result.dimensions[1] == n &&
        !result.scalar && result.type == NumericType::Float32, "matrix result metadata");
  auto actual = take(runtime.download(result.data), "matrix output");
  check(actual.size() == std::size_t(m) * n, "matrix output count");
  for (unsigned row = 0; row < m; ++row)
    for (unsigned column = 0; column < n; ++column) {
      double expected = 0, magnitude = 0;
      for (unsigned inner = 0; inner < k; ++inner) {
        const double product = double(a[row * k + inner]) * double(b[inner * n + column]);
        expected += product;
        magnitude += std::abs(product);
      }
      const auto value = actual[row * n + column];
      if (m == 1 && n == 1 && k == 1)
        check(std::bit_cast<std::uint32_t>(value) == std::bit_cast<std::uint32_t>(a[0]),
              "single-product full float32 precision");
      const double bound = 4 * std::numeric_limits<float>::epsilon() * std::max(1u, k) * magnitude;
      check(std::isfinite(value) && std::abs(double(value) - expected) <= bound,
            "independent double/magnitude reference");
    }
}
void corpus(unsigned device, bool validation, const char *path, bool tiled) {
  auto runtime = take(v::Runtime::create({.validation = validation, .deviceIndex = device,
                                         .enableTiledMatmul = tiled}), "corpus context");
  if (tiled) {
    std::array<v::Kernel, 4> matrices, layouts;
    std::array<std::thread, 4> threads;
    for (unsigned i = 0; i < threads.size(); ++i)
      threads[i] = std::thread([&, i] {
        matrices[i] = take(runtime->loadMatmulKernel(path), "concurrent matrix plan");
        layouts[i] = take(runtime->loadBuiltinTransposeFloat32(), "concurrent layout plan");
      });
    for (auto &thread : threads) thread.join();
    for (unsigned i = 0; i < threads.size(); ++i)
      check(matrices[i].tiledMatmul() && runtime->owns(layouts[i]), "complete concurrent built-in publication");
  }
  auto prepared = program(*runtime, path, tiled);
  if (tiled) {
    std::vector<v::Kernel> aliases;
    for (unsigned i = 0; i < 100; ++i) {
      aliases.push_back(take(runtime->loadMatmulKernel(path), "shared matrix plan"));
      aliases.push_back(take(runtime->loadBuiltinTransposeFloat32(), "shared layout plan"));
    }
    check(aliases.size() == 200, "built-in sharing does not consume a kernel slot per program");
  }
  for (auto shape : {std::array{0u, 0u, 0u}, {0u, 5u, 7u}, {3u, 0u, 7u}, {3u, 5u, 0u},
                     {1u, 1u, 1u}, {1u, 1u, 67u}, {3u, 1u, 7u}, {1u, 5u, 1u},
                     {2u, 2u, 3u}, {31u, 65u, 33u}, {64u, 64u, 32u}, {65u, 67u, 63u},
                     {128u, 128u, 96u}, {33u, 7u, 257u}, {19u, 35u, 129u}, {2u, 3u, 4097u}}) {
    const auto [m, n, k] = shape;
    {
      std::vector<float> a(m * k), b(k * n);
      for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = float(int(i % 31) - 15) / 32 + std::ldexp(float(i % 7), -20);
      for (std::size_t i = 0; i < b.size(); ++i)
        b[i] = float(int(i % 23) - 11) / 16 - std::ldexp(float(i % 5), -19);
      if (m == 1 && n == 1 && k == 1) {
        a[0] = 1.0f + std::ldexp(1.0f, -20);
        b[0] = 1;
      }
      std::array inputs{upload(*runtime, a, m, k), upload(*runtime, b, k, n)};
      auto execution = take(prepared.start(*runtime, inputs, {}), "start matrix program");
      unsigned stages = 0;
      while (!execution.finished()) {
        auto dispatch = take(execution.next(*runtime), "matrix next");
        if (!dispatch) continue;
        auto &work = *dispatch;
        auto result = take(runtime->execute(work.kernel, work.inputs, work.parameters,
                                            work.outputBytes, work.groups), "matrix dispatch");
        check(bool(execution.accept(*runtime, std::move(result))), "matrix acceptance");
        ++stages;
      }
      check(stages == (tiled && m && n > 1 && k > 1 ? 2u : 1u), "layout stage only when necessary");
      verify(*runtime, take(execution.result(), "matrix result"), a, b, m, n, k);
      check(take(runtime->download(inputs[0].data), "unchanged A") == a &&
            take(runtime->download(inputs[1].data), "unchanged B") == b, "immutable matrix inputs");
    }
    check(!runtime->retainedBytes(), "matrix/transpose workspace reclaimed");
  }
  check(!runtime->validationErrors(), "matrix corpus zero validation errors");
}
void admission(unsigned device, bool validation, const char *path) {
  struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("kalandra-tiled-admission-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { check(std::filesystem::create_directory(root), "exclusive admission fixtures"); }
    ~Scratch() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  } scratch;
  const auto copy = [&](const std::filesystem::path &source, const std::string &name) {
    check(std::filesystem::copy_file(source, scratch.root / name), "copy admission fixture");
  };
  const auto corrupt = [&](const std::string &name) {
    std::ofstream file(scratch.root / name, std::ios::binary);
    const std::array<std::uint32_t, 5> invalid{};
    file.write(reinterpret_cast<const char *>(invalid.data()), sizeof(invalid));
    check(bool(file), "write corrupt admission fixture");
  };
  const std::filesystem::path fixed = std::string(path) + ".fixed-tree.spv";
  copy(path, "complete.spv");
  copy(fixed, "complete.spv.fixed-tree.spv");
  // A valid numerical backup must not disguise an absent performance backup.
  copy(path, "missing-backup.spv");
  copy(fixed, "missing-backup.spv.software.spv");
  copy(path, "corrupt-backup.spv");
  corrupt("corrupt-backup.spv.fixed-tree.spv");
  corrupt("corrupt-primary.spv");
  copy(fixed, "corrupt-primary.spv.fixed-tree.spv");
  copy(fixed, "corrupt-primary.spv.software.spv");
  copy(fixed, "missing-primary.spv.fixed-tree.spv");
  copy(fixed, "missing-primary.spv.software.spv");
  for (bool fallback : {false, true}) {
    auto runtime = take(v::language::Runtime::create({.validation = validation,
        .defaultDevice = device, .allowSoftwareFallback = fallback,
        .enableTiledMatmul = false}), "forced algorithm ceiling");
    auto selected = take(runtime->device(), "admission device");
    const auto load = [&](const std::string &name) {
      return runtime->loadNumericMatmulKernel((scratch.root / name).string(), selected,
                                              NumericType::Float32);
    };
    auto complete = take(load("complete.spv"), "native fixed-tree algorithm fallback");
    check(!complete.usesTiledMatmul() && !complete.usedSoftwareFallback(),
          "algorithm fallback remains hardware arithmetic under either numeric policy");
    for (const auto &[name, error] : std::array{
             std::pair{"missing-backup.spv", v::Error::Unsupported},
             std::pair{"corrupt-backup.spv", v::Error::Invalid},
             std::pair{"corrupt-primary.spv", v::Error::Invalid},
             std::pair{"missing-primary.spv", v::Error::Unavailable}}) {
      auto result = load(name);
      check(!result && result.error() == error, "matrix artifacts fail closed without numerical substitution");
    }
    check(!runtime->stats().bytes, "admission owns no tensor buffers");
  }
}
struct Hold {
  v::AsyncRuntime &async;
  explicit Hold(v::AsyncRuntime &value) : async(value) { async.holdCompletions(true); }
  ~Hold() { async.holdCompletions(false); }
};
void managed(unsigned device, bool validation, const char *path) {
  for (unsigned workers : {1u, 2u, 4u})
    for (unsigned mode : {0u, 1u, 2u, 3u}) {
      std::shared_ptr<v::Runtime> runtime = take(v::Runtime::create({.validation = validation,
          .deviceIndex = device}), "managed matrix context");
      auto prepared = program(*runtime, path, true);
      std::vector<float> a(65 * 63, .125f), b(63 * 67, .25f);
      std::array inputs{upload(*runtime, a, 65, 63), upload(*runtime, b, 63, 67)};
      const auto baseline = runtime->retainedBytes();
      auto async = v::AsyncRuntime::create(runtime);
      auto core = TaskCore::create({.workers = workers});
      check(bool(async) && bool(core), "managed services");
      Hold hold(*async);
      auto group = core->openRoot();
      auto task = async->executeProgram(*core, group, prepared, inputs, {});
      check(bool(task) && core->close(group), "managed matrix submit");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!async->stats().held && std::chrono::steady_clock::now() < deadline) {
        core->step();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(async->stats().held == 1 && core->stats().externalPending == 1 &&
            runtime->retainedBytes() > baseline, "transpose workspace pinned before acknowledgement");
      const auto retained = runtime->retainedBytes();
      if (mode == 1 || mode == 2) {
        runtime->injectFailure(v::FailurePoint::QueueSubmit);
        check(core->cancel(group, mode == 2) && !core->settled(group) &&
              runtime->retainedBytes() == retained, "cancel/deadline retains layout workspace");
      } else if (mode == 3)
        runtime->injectFailure(v::FailurePoint::PipelineCreate);
      async->holdCompletions(false);
      check(core->drain(), "managed matrix drain");
      if (mode == 1 || mode == 2) {
        check(core->failure(task).kind == TaskFailure::Cancelled && async->stats().stages == 1,
              "cancellation prevents matrix submission after layout");
        check(core->failure(group).kind == (mode == 2 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "group deadline distinction");
        runtime->injectFailure(v::FailurePoint::None);
      } else if (mode == 3)
        check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 2 &&
              async->stats().stages == 2, "matrix pipeline failure after completed layout");
      else {
        Invocation invocation;
        alignas(v::TensorValue) std::byte memory[sizeof(v::TensorValue)];
        check(core->observe(task, v::AsyncRuntime::tensorLayout(), invocation, memory), "observe matrix");
        auto *value = std::launder(reinterpret_cast<v::TensorValue *>(memory));
        verify(*runtime, *value, a, b, 65, 67, 63);
        value->~TensorValue();
        check(async->stats().stages == 2, "two-stage managed matrix");
      }
      check(core->release(group) && !core->stats().records && !core->stats().externalPending &&
            !core->stats().retainedBytes && !async->stats().pending && !async->stats().captureBytes &&
            runtime->retainedBytes() == baseline && !runtime->validationErrors(), "all matrix ownership retired");
    }
}
} // namespace
int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) return 2;
  const unsigned device = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
  const bool validation = argc < 4 || std::string_view(argv[3]) != "--no-validation";
  try {
    auto anchor = take(v::Runtime::create({.validation = validation, .deviceIndex = device}), "ICD anchor");
    corpus(device, validation, argv[1], true);
    corpus(device, validation, argv[1], false);
    admission(device, validation, argv[1]);
    managed(device, validation, argv[1]);
    std::puts("PASS: 32 matrix geometries/algorithm profiles and 10 admission policies and 12 managed layout/cancel/deadline/failure cases");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
