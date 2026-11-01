// Numerical-library comparison, not a source-level loop/language ranking.
#include "Language.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace m = v::language;
using Clock = std::chrono::steady_clock;
using Matrix = CpuTensor<NumericType::Float32, 2>;
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Matrix *, Invocation *);
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T, class E> T take(std::expected<T, E> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, (long long)value.error());
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
struct Blas {
  using Sgemm = void (*)(int, int, int, int, int, int, float, const float *, int, const float *,
                         int, float, float *, int);
  using Dgemm = void (*)(int, int, int, int, int, int, double, const double *, int, const double *,
                         int, double, double *, int);
  void *library = nullptr;
  Sgemm sgemm;
  Dgemm dgemm;
  std::string configuration;
  int threads;
  template <class F> F symbol(const char *name) {
    auto result = reinterpret_cast<F>(dlsym(library, name));
    check(bool(result), name);
    return result;
  }
  Blas(const char *path, int count) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    check(bool(library), "existing OpenBLAS library");
    configuration = symbol<const char *(*)()>("openblas_get_config")();
    check(configuration.find("USE64BITINT") == std::string::npos, "LP64 reference ABI required");
    symbol<void (*)(int)>("openblas_set_num_threads")(count);
    threads = symbol<int (*)()>("openblas_get_num_threads")();
    check(threads == count, "actual BLAS thread count");
    if (symbol<int (*)()>("openblas_get_parallel")() == 2) {
      check(symbol<int (*)()>("omp_get_max_threads")() == count &&
                !symbol<int (*)()>("omp_get_dynamic")(),
            "actual OpenMP reference team limit");
    }
    sgemm = symbol<Sgemm>("cblas_sgemm");
    dgemm = symbol<Dgemm>("cblas_dgemm");
  }
  ~Blas() {
    if (library)
      dlclose(library);
  }
};
struct Data {
  unsigned n, pattern;
  std::array<std::vector<float>, 2> a;
  std::vector<float> b;
  Data(unsigned side, unsigned kind)
      : n(side), pattern(kind), a{std::vector<float>(n * n), std::vector<float>(n * n)}, b(n * n) {
    for (unsigned variant = 0; variant < 2; ++variant)
      for (std::size_t i = 0; i < b.size(); ++i)
        a[variant][i] = left(i, variant);
    for (std::size_t i = 0; i < b.size(); ++i)
      b[i] = right(i);
  }
  float left(std::size_t i, unsigned variant) const {
    i += variant * 3;
    return pattern ? float(int((i * 37) % 101) - 50) / 37.0f : float(int(i % 17) - 8) / 16.0f;
  }
  float right(std::size_t i) const {
    return pattern ? float(int((i * 43) % 101) - 50) / 29.0f : float(int(i % 13) - 6) / 16.0f;
  }
  void immutable() const {
    for (unsigned variant = 0; variant < 2; ++variant)
      for (std::size_t i = 0; i < b.size(); ++i)
        check(a[variant][i] == left(i, variant), "immutable left input");
    for (std::size_t i = 0; i < b.size(); ++i)
      check(b[i] == right(i), "immutable right input");
  }
  std::uint64_t fingerprint() const {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto *input : {&a[0], &a[1], &b})
      for (float x : *input) {
        value ^= std::bit_cast<std::uint32_t>(x);
        value *= 1099511628211ULL;
      }
    return value;
  }
};
struct Result {
  std::optional<Invocation> cpu;
  Matrix matrix{};
  std::unique_ptr<float[]> blas;
  m::Buffer gpu;
  void clear() {
    cpu.reset();
    blas.reset();
    gpu = {};
    matrix = {};
  }
};
struct Engine {
  const Data &data;
  std::string mode, device;
  bool transfers;
  std::unique_ptr<Blas> blas;
  std::unique_ptr<m::Runtime> runtime;
  std::unique_ptr<TaskCore> core;
  m::NumericExecutable plan;
  m::Device selected;
  std::array<m::Buffer, 2> hostA, residentA;
  m::Buffer hostB, residentB;
  m::Stats baseline{};
  Engine(const Data &input, std::string kind, const char *bundle, const char *library, int threads)
      : data(input), mode(std::move(kind)), transfers(mode == "gpu-transfers") {
    if (mode == "blas") {
      blas = std::make_unique<Blas>(library, threads);
      return;
    }
    if (mode == "cpu")
      return;
    check(mode == "gpu-resident" || transfers, "known engine");
    runtime = take(m::Runtime::create(), "GPU registry");
    selected = take(runtime->device(), "physical GPU");
    device = selected.name();
    core = TaskCore::create({.workers = 1});
    check(bool(core), "task core");
    auto kernel = take(runtime->loadNumericMatmulKernel(std::string(bundle) + "/kernel0.spv",
                                                        selected, NumericType::Float32),
                       "source matrix kernel");
    const bool optimized = std::string(bundle).find("optimized") != std::string::npos;
    check(kernel.usesTiledMatmul() == optimized, "actual requested GPU algorithm");
    const std::array<std::uint32_t, 2> ids{0, 1};
    const std::array<m::NumericInstruction, 1> code{
        {{.kind = NumericProgramKind::Matmul, .kernel = kernel, .inputs = ids, .output = 2}}};
    plan = take(runtime->prepareNumericProgram(2, code, 2, NumericType::Float32, false, selected),
                "prepared source graph");
    Invocation producer;
    const std::array<std::int64_t, 2> shape{data.n, data.n};
    for (unsigned i = 0; i < 2; ++i) {
      auto tensor =
          take(copyCpuTensor<NumericType::Float32, 2>(producer, NumericType::Float32,
                                                      std::as_bytes(std::span(data.a[i])), shape),
               "CPU left descriptor");
      hostA[i] = take(runtime->fromCpu(producer, tensor.view()), "owned host left");
    }
    auto tensor = take(copyCpuTensor<NumericType::Float32, 2>(
                           producer, NumericType::Float32, std::as_bytes(std::span(data.b)), shape),
                       "CPU right descriptor");
    hostB = take(runtime->fromCpu(producer, tensor.view()), "owned host right");
    producer.settle();
    check(!producer.live(), "producer retired while transfers retain inputs");
    if (!transfers) {
      for (unsigned i = 0; i < 2; ++i)
        residentA[i] = transfer(hostA[i], false);
      residentB = transfer(hostB, false);
    }
    baseline = runtime->stats();
  }
  m::Buffer finish(TaskHandle task, GroupHandle group) {
    check(bool(task) && core->close(group) && core->drain(), "completed managed task");
    Invocation observer;
    alignas(m::Buffer) std::byte bytes[sizeof(m::Buffer)];
    check(core->observe(task, m::Runtime::bufferLayout(), observer, bytes), "owned task result");
    auto value = std::launder(reinterpret_cast<m::Buffer *>(bytes));
    auto result = *value;
    std::destroy_at(value);
    check(core->release(group), "task group retired");
    auto s = core->stats();
    check(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending,
          "task ownership/quiescence");
    return result;
  }
  m::Buffer transfer(const m::Buffer &value, bool download) {
    auto group = core->openRoot();
    auto task = download ? runtime->download(*core, group, value)
                         : runtime->upload(*core, group, value, selected);
    return finish(task, group);
  }
  void call(Result &out, unsigned variant) {
    out.clear();
    if (mode == "cpu") {
      out.cpu.emplace();
      Matrix a{const_cast<float *>(data.a[variant].data()),
               const_cast<float *>(data.a[variant].data()),
               0,
               {data.n, data.n},
               {data.n, 1}};
      Matrix b{const_cast<float *>(data.b.data()),
               const_cast<float *>(data.b.data()),
               0,
               {data.n, data.n},
               {data.n, 1}};
      _mlir_ciface_kalandra_test_entry(&out.matrix, &a, &b, &*out.cpu);
      check(!out.cpu->status(), "CPU checked matrix invocation");
      check(out.matrix.sizes[0] == data.n && out.matrix.sizes[1] == data.n &&
                out.matrix.strides[0] == data.n && out.matrix.strides[1] == 1,
            "CPU output metadata");
    } else if (mode == "blas") {
      out.blas = std::make_unique_for_overwrite<float[]>(data.b.size());
      blas->sgemm(101, 111, 111, data.n, data.n, data.n, 1, data.a[variant].data(), data.n,
                  data.b.data(), data.n, 0, out.blas.get(), data.n);
    } else {
      auto a = transfers ? transfer(hostA[variant], false) : residentA[variant];
      auto b = transfers ? transfer(hostB, false) : residentB;
      auto group = core->openRoot();
      out.gpu = finish(runtime->executeNumericProgram(*core, group, plan, std::array{a, b}), group);
      if (transfers)
        out.gpu = transfer(out.gpu, true);
      check(out.gpu.rank == 2 && out.gpu.dimensions[0] == data.n && out.gpu.dimensions[1] == data.n,
            "GPU output metadata");
    }
  }
  void quiescent() const {
    if (!runtime)
      return;
    auto s = runtime->stats();
    check(s.bytes == baseline.bytes && !s.inFlight && s.argumentBytes == baseline.argumentBytes,
          "bounded invocation resources retired");
  }
};
struct Reference {
  std::array<std::vector<double>, 2> expected, magnitude;
  const Data &data;
  double maxError = 0, maxFraction = 0;
  std::uint64_t cells = 0;
  Reference(const Data &input, const char *library) : data(input) {
    Blas blas(library, 1);
    std::vector<double> b(data.b.begin(), data.b.end()), absB = b;
    for (auto &x : absB)
      x = std::abs(x);
    for (unsigned variant = 0; variant < 2; ++variant) {
      std::vector<double> a(data.a[variant].begin(), data.a[variant].end()), absA = a;
      for (auto &x : absA)
        x = std::abs(x);
      expected[variant].resize(b.size());
      magnitude[variant].resize(b.size());
      blas.dgemm(101, 111, 111, data.n, data.n, data.n, 1, a.data(), data.n, b.data(), data.n, 0,
                 expected[variant].data(), data.n);
      blas.dgemm(101, 111, 111, data.n, data.n, data.n, 1, absA.data(), data.n, absB.data(), data.n,
                 0, magnitude[variant].data(), data.n);
      // Independent scalar dots also check the reference library/layout itself.
      for (std::size_t index = 0; index < std::min(std::size_t(257), b.size()); ++index) {
        const auto row = (index * 37) % data.n, col = (index * 53 + 1) % data.n;
        double exact = 0;
        for (unsigned k = 0; k < data.n; ++k)
          exact += double(data.a[variant][row * data.n + k]) * double(data.b[k * data.n + col]);
        check(std::abs(exact - expected[variant][row * data.n + col]) <=
                  32 * std::numeric_limits<double>::epsilon() * data.n *
                      magnitude[variant][row * data.n + col],
              "independent scalar/double library reference");
      }
    }
  }
  double verify(Engine &engine, Result &result, unsigned variant) {
    m::Buffer downloaded;
    std::span<const float> values;
    if (result.cpu)
      values = {result.matrix.aligned + result.matrix.offset, data.b.size()};
    else if (result.blas)
      values = {result.blas.get(), data.b.size()};
    else {
      downloaded = result.gpu.onDevice() ? engine.transfer(result.gpu, true) : result.gpu;
      auto bytes = take(v::hostBytes(downloaded), "observed tensor bytes");
      check(bytes.size() == data.b.size() * sizeof(float), "full output byte extent");
      values = {reinterpret_cast<const float *>(bytes.data()), data.b.size()};
    }
    double checksum = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
      const double error = std::abs(double(values[i]) - expected[variant][i]);
      const double bound =
          4 * std::numeric_limits<float>::epsilon() * data.n * magnitude[variant][i];
      check(std::isfinite(values[i]) && error <= bound,
            "full output independent double/magnitude numerical bound");
      maxError = std::max(maxError, error);
      maxFraction = std::max(maxFraction, bound ? error / bound : 0);
      checksum += values[i];
      ++cells;
    }
    return checksum;
  }
};
int run(int argc, char **argv) {
  if (argc != 8)
    return 2;
  const std::string mode = argv[1];
  const unsigned n = std::stoul(argv[2]), pattern = std::stoul(argv[3]);
  const int threads = std::stoi(argv[4]), rounds = std::stoi(argv[7]);
  if (!n || n > 1024 || pattern > 1 || threads < 1 || threads > 64 || rounds < 1 || rounds > 9)
    return 2;
  Data data(n, pattern);
  const auto initialize = Clock::now();
  Engine engine(data, mode, argv[5], argv[6], threads);
  const double initialization = elapsed(initialize);
  std::array<Result, 2> outputs;
  const auto cold = Clock::now();
  engine.call(outputs[0], 0);
  const double first = elapsed(cold);
  Reference reference(data, argv[6]);
  reference.verify(engine, outputs[0], 0);
  outputs[0].clear();
  engine.quiescent();
  // Reference generation must not change the selected timed BLAS thread policy.
  if (engine.blas)
    engine.blas = std::make_unique<Blas>(argv[6], threads);
  const auto calibration = Clock::now();
  for (unsigned i = 0; i < 2; ++i)
    engine.call(outputs[i], i);
  const double warm = elapsed(calibration) / 2;
  reference.verify(engine, outputs[0], 0);
  reference.verify(engine, outputs[1], 1);
  const auto iterations =
      std::clamp<unsigned>(unsigned(std::ceil(2e8 / std::max(1.0, warm) / 2)) * 2, 2, 4096);
  std::printf("{\"kind\":\"initialization\",\"engine\":\"%s\",\"n\":%u,\"pattern\":%u,\"threads\":%"
              "d,\"initialization_ns\":%.0f,\"first_call_ns\":%.0f,\"calibration_ns_per_call\":%."
              "0f,\"iterations\":%u,\"input_fingerprint\":\"%016llx\",\"device\":\"%s\"}\n",
              mode.c_str(), n, pattern, threads, initialization, first, warm, iterations,
              (unsigned long long)data.fingerprint(), engine.device.c_str());
  for (int round = 0; round < rounds; ++round) {
    const auto start = Clock::now();
    for (unsigned i = 0; i < iterations; ++i)
      engine.call(outputs[i % 2], i % 2);
    const double ns = elapsed(start);
    const double checksum =
        reference.verify(engine, outputs[0], 0) + reference.verify(engine, outputs[1], 1);
    std::printf("{\"kind\":\"sample\",\"round\":%d,\"engine\":\"%s\",\"n\":%u,\"pattern\":%u,"
                "\"threads\":%d,\"ns\":%.0f,\"iterations\":%u,\"checksum\":%.17g}\n",
                round, mode.c_str(), n, pattern, threads, ns, iterations, checksum);
    std::fflush(stdout);
  }
  for (auto &result : outputs)
    result.clear();
  engine.quiescent();
  data.immutable();
  std::printf("{\"kind\":\"correctness\",\"validated_cells\":%llu,\"max_abs_error\":%.17g,\"max_"
              "bound_fraction\":%.17g,\"immutable_inputs\":true,\"resources_retired\":true}\n",
              (unsigned long long)reference.cells, reference.maxError, reference.maxFraction);
  return 0;
}
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
