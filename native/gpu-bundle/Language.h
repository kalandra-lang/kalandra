#pragma once
#include "../cuda/Language.h"
#include "../vulkan/Language.h"
#include "Selection.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <variant>

namespace kalandra::runtime::bundle {
namespace cl = cuda::language;
namespace vk = vulkan::language;
using Error = cl::Error;
using Format = NumericType;
using cl::storageFormat;
using NumericInstructionKind = NumericProgramKind;
using ElementwiseSignature = cl::ElementwiseSignature;
using ElementwiseBinding = cl::ElementwiseBinding;

struct Buffer {
  std::variant<cl::Buffer, vk::Buffer> value;
  explicit operator bool() const {
    return std::visit([](auto &v) { return bool(v); }, value);
  }
  bool onDevice() const {
    return std::visit([](auto &v) { return v.onDevice(); }, value);
  }
  auto shape() const {
    return std::visit([](auto &v) { return v.shape(); }, value);
  }
  Format format() const {
    return std::visit([](auto &v) { return v.format(); }, value);
  }
};
using Device = std::variant<cl::Device, vk::Device>;
using Kernel = std::variant<cl::Kernel, vk::Kernel>;
struct NumericExecutable {
  std::variant<cl::NumericExecutable, vk::NumericExecutable> value;
  bool on(const Device &device) const {
    if (auto cuda = std::get_if<cl::NumericExecutable>(&value))
      return std::holds_alternative<cl::Device>(device) && cuda->on(std::get<cl::Device>(device));
    return std::holds_alternative<vk::Device>(device) &&
           std::get<vk::NumericExecutable>(value).on(std::get<vk::Device>(device));
  }
};
struct NumericInstruction {
  NumericInstructionKind kind;
  Kernel kernel{};
  std::span<const std::uint32_t> inputs{};
  std::span<const std::uint64_t> scalars{};
  std::uint32_t output = UINT32_MAX, target = 0, workPerElement = 0;
};

struct Module {
  const char *stem;
  bool matmul;
};

class Runtime {
  using Context = std::variant<std::unique_ptr<cl::Runtime>, std::unique_ptr<vk::Runtime>>;
  template <class R, class C, class V>
  using For = std::conditional_t<std::is_same_v<std::remove_reference_t<R>, cl::Runtime>, C, V>;

  const Selection<Context, Error>::Result &selected() {
    return selection_.get(
        []() -> std::string_view {
          const char *forced = std::getenv("KALANDRA_GPU_BACKEND");
          if (!forced || !*forced)
            return {};
          std::string_view value(forced);
          if (value != "cuda" && value != "vulkan")
            std::fprintf(stderr,
                         "Kalandra GPU: invalid KALANDRA_GPU_BACKEND (expected cuda or vulkan)\n");
          return value;
        },
        [&](Backend backend) -> std::expected<Context, Error> {
          auto result = backend == Backend::Cuda
                            ? admit<cl::Runtime, cl::Options, cuda::TensorPublication>(backend)
                            : admit<vk::Runtime, vk::Options, vulkan::TensorPublication>(backend);
          if (!result)
            std::fprintf(stderr, "Kalandra GPU: %s admission failed (%lld)\n", name(backend).data(),
                         static_cast<long long>(result.error()));
          return result;
        });
  }
  template <class R, class Options, class Publication>
  std::expected<Context, Error> admit(Backend backend) {
    using Tensor = For<R, cl::Buffer, vk::Buffer>;
    static const Publication publication{[](const TaskEntry &native) noexcept -> const TaskEntry & {
                                           static const TaskEntry published = [&] {
                                             auto entry = native;
                                             entry.result = &bufferLayout();
                                             return entry;
                                           }();
                                           return published;
                                         },
                                         [](Task &task, const Tensor &tensor) noexcept {
                                           Buffer item{tensor};
                                           task.complete(&item);
                                         }};
    Options options;
    options.allowSoftwareFallback = options_.allowSoftwareFallback;
    options.publication = &publication;
    using Driver = For<R, cuda::Runtime, vulkan::Runtime>;
    try {
      auto devices = Driver::enumerate(false, true);
      if (!devices)
        return std::unexpected(static_cast<Error>(devices.error()));
      Error last = Error::Unavailable;
      for (std::size_t index = 0; index < devices->size(); ++index) {
        // Logical GPU zero is the first compatible physical adapter. Its stable
        // UUID is also validated by the concrete driver when opening the context.
        auto ordered = *devices;
        std::rotate(ordered.begin(), ordered.begin() + index, ordered.begin() + index + 1);
        auto runtime = R::create(options, std::move(ordered));
        if (runtime) {
          for (const auto &module : modules_) {
            auto path = directory_ / module.stem;
            path += backend == Backend::Cuda ? ".ptx" : ".spv";
            auto compatible = (*runtime)->admitModule(path, module.matmul);
            if (!compatible) {
              runtime = std::unexpected(compatible.error());
              break;
            }
          }
        }
        if (runtime) {
          if (std::getenv("KALANDRA_GPU_TRACE")) {
            auto device = (*runtime)->device();
            if (device)
              std::fprintf(stderr, "Kalandra GPU: selected %s (%.*s)\n", name(backend).data(),
                           int(device->name().size()), device->name().data());
          }
          active_.store(true, std::memory_order_release);
          return Context{std::move(*runtime)};
        }
        last = static_cast<Error>(runtime.error());
        if (last != Error::Unsupported && last != Error::Unavailable)
          break;
      }
      return std::unexpected(last);
    } catch (const std::bad_alloc &) {
      return std::unexpected(Error::Allocation);
    } catch (const std::filesystem::filesystem_error &) {
      return std::unexpected(Error::Invalid);
    }
  }
  template <class T, class F> std::expected<T, Error> withRuntime(F &&fn) {
    auto &runtime = selected();
    if (!runtime)
      return std::unexpected(runtime.error());
    try {
      return std::visit(
          [&](auto &pointer) -> std::expected<T, Error> {
            auto result = fn(*pointer);
            if (!result)
              return std::unexpected(static_cast<Error>(result.error()));
            return T{std::move(*result)};
          },
          *runtime);
    } catch (const std::bad_alloc &) {
      return std::unexpected(Error::Allocation);
    }
  }
  template <class F>
  std::expected<Kernel, Error> load(const std::string &path, const Device &device, F &&fn) {
    return withRuntime<Kernel>([&](auto &runtime) {
      using R = decltype(runtime);
      using D = For<R, cl::Device, vk::Device>;
      using K = For<R, cl::Kernel, vk::Kernel>;
      using E = For<R, cl::Error, vk::Error>;
      auto *target = std::get_if<D>(&device);
      if (!target)
        return std::expected<K, E>(std::unexpected(E::Residency));
      auto module = std::filesystem::path(std::u8string(path.begin(), path.end()));
      module.replace_extension(std::is_same_v<D, cl::Device> ? ".ptx" : ".spv");
      const auto utf8 = module.u8string();
      return fn(runtime, std::string_view(reinterpret_cast<const char *>(utf8.data()), utf8.size()),
                *target);
    });
  }

public:
  void setModules(std::string_view directory, std::span<const Module> modules) {
    directory_ = std::u8string(directory.begin(), directory.end());
    modules_ = modules;
  }
  struct Options {
    bool allowSoftwareFallback = false;
  };
  static std::expected<std::unique_ptr<Runtime>, Error> create(Options options) noexcept {
    auto result = std::unique_ptr<Runtime>(new (std::nothrow) Runtime(options));
    if (!result)
      return std::unexpected(Error::Allocation);
    return result;
  }
  static auto create() noexcept { return create({}); }
  auto device(std::uint32_t index = 0) {
    return withRuntime<Device>([&](auto &r) { return r.device(index); });
  }
  auto fromCpu(const Invocation &context, CpuTensorView view) {
    return withRuntime<Buffer>([&](auto &r) { return r.fromCpu(context, view); });
  }
  template <NumericType Element, std::size_t Rank>
  auto toCpuTyped(Invocation &context, const Buffer &buffer) {
    return withRuntime<CpuTensor<Element, Rank>>([&](auto &r) {
      using B = For<decltype(r), cl::Buffer, vk::Buffer>;
      using E = For<decltype(r), cl::Error, vk::Error>;
      if (auto value = std::get_if<B>(&buffer.value))
        return r.template toCpuTyped<Element, Rank>(context, *value);
      return std::expected<CpuTensor<Element, Rank>, E>(std::unexpected(E::Residency));
    });
  }
  static const ValueLayout &bufferLayout() {
    static const ValueLayout layout{
        0x4b424e444c01, sizeof(Buffer), alignof(Buffer),
        [](Invocation &, void *out, const Invocation &, const void *in) noexcept {
          new (out) Buffer(*static_cast<const Buffer *>(in));
          return true;
        },
        [](void *out) noexcept { std::destroy_at(static_cast<Buffer *>(out)); }};
    return layout;
  }
  static auto arrayResult(const Buffer &b) {
    return std::visit([](auto &v) { return v.arrayResult; }, b.value);
  }
  static auto workgroupOutcome(const Buffer &b) {
    return std::visit([](auto &v) { return v.workgroupOutcome; }, b.value);
  }
  static std::expected<std::span<const std::byte>, Error> arrayPageBytes(const Buffer &b) {
    return std::visit(
        [](auto &v) -> std::expected<std::span<const std::byte>, Error> {
          using R = std::conditional_t<std::is_same_v<std::decay_t<decltype(v)>, cl::Buffer>,
                                       cl::Runtime, vk::Runtime>;
          auto bytes = R::arrayPageBytes(v);
          if (!bytes)
            return std::unexpected(static_cast<Error>(bytes.error()));
          return *bytes;
        },
        b.value);
  }
  auto loadNumericGraphKernel(const std::string &path, const Device &d, NumericType t,
                              ElementwiseSignature s, std::span<const NumericType> tensors,
                              std::span<const NumericType> scalars,
                              std::span<const std::uint32_t> reads,
                              std::span<const std::uint8_t> roles) {
    return load(path, d, [&](auto &r, auto p, auto dev) {
      using Binding = For<decltype(r), cl::ElementwiseBinding, vk::ElementwiseBinding>;
      return r.loadNumericGraphKernel(p, dev, t,
                                      {s.tensors, s.scalars, static_cast<Binding>(s.binding)},
                                      tensors, scalars, reads, roles);
    });
  }
  auto loadNumericReductionKernel(const std::string &p, const Device &d, NumericType t) {
    return load(p, d, [&](auto &r, auto path, auto dev) {
      return r.loadNumericReductionKernel(path, dev, t);
    });
  }
  auto loadNumericMatmulKernel(const std::string &p, const Device &d, NumericType t) {
    return load(p, d, [&](auto &r, auto path, auto dev) {
      return r.loadNumericMatmulKernel(path, dev, t);
    });
  }
  auto loadTensorTransformKernel(const std::string &p, const Device &d, TensorTransformKind kind,
                                 NumericType t, bool epsilon) {
    return load(p, d, [&](auto &r, auto path, auto dev) {
      return r.loadTensorTransformKernel(path, dev, kind, t, epsilon);
    });
  }
  auto loadWorkgroupKernel(const std::string &p, const Device &d, NumericType t,
                           std::span<const NumericType> tensors,
                           std::span<const NumericType> scalars, const WorkgroupLaunch &launch) {
    return load(p, d, [&](auto &r, auto path, auto dev) {
      return r.loadWorkgroupKernel(path, dev, t, tensors, scalars, launch);
    });
  }
  auto prepareNumericProgram(std::uint32_t count, std::span<const NumericInstruction> instructions,
                             std::uint32_t result, NumericType type, bool scalar, bool boolean,
                             const Device &d) {
    return withRuntime<NumericExecutable>([&](auto &r) {
      using R = decltype(r);
      using D = For<R, cl::Device, vk::Device>;
      using K = For<R, cl::Kernel, vk::Kernel>;
      using E = For<R, cl::Error, vk::Error>;
      using Exec = For<R, cl::NumericExecutable, vk::NumericExecutable>;
      std::vector<For<R, cl::NumericInstruction, vk::NumericInstruction>> code;
      auto target = std::get_if<D>(&d);
      if (!target)
        return std::expected<Exec, E>(std::unexpected(E::Residency));
      if (instructions.size_bytes() > 1024 * 1024)
        return std::expected<Exec, E>(std::unexpected(E::Budget));
      code.reserve(instructions.size());
      for (auto &op : instructions) {
        auto k = std::get_if<K>(&op.kernel);
        if (!k && std::visit([](auto &native) { return bool(native); }, op.kernel))
          return std::expected<Exec, E>(std::unexpected(E::Residency));
        code.push_back({op.kind, k ? *k : K{}, op.inputs, op.scalars, op.output, op.target,
                        op.workPerElement});
      }
      return r.prepareNumericProgram(count, code, result, type, scalar, boolean, *target);
    });
  }
  auto prepareNumericProgram(std::uint32_t count, std::span<const NumericInstruction> code,
                             std::uint32_t result, NumericType type, bool scalar, const Device &d) {
    return prepareNumericProgram(count, code, result, type, scalar, false, d);
  }
  template <class Caller>
  TaskHandle upload(Caller &c, GroupHandle g, const Buffer &b, const Device &d) {
    return withTask(c, [&](auto &r) {
      using R = decltype(r);
      return r.upload(c, g, std::get<For<R, cl::Buffer, vk::Buffer>>(b.value),
                      std::get<For<R, cl::Device, vk::Device>>(d));
    });
  }
  template <class Caller> TaskHandle download(Caller &c, GroupHandle g, const Buffer &b) {
    return withTask(c, [&](auto &r) {
      return r.download(c, g, std::get<For<decltype(r), cl::Buffer, vk::Buffer>>(b.value));
    });
  }
  TaskHandle downloadArrayPage(Task &c, GroupHandle g, const Buffer &b, std::uint64_t offset,
                               std::uint64_t words) {
    return withTask(c, [&](auto &r) {
      return r.downloadArrayPage(c, g, std::get<For<decltype(r), cl::Buffer, vk::Buffer>>(b.value),
                                 offset, words);
    });
  }
  template <class Caller>
  TaskHandle executeNumericProgram(Caller &c, GroupHandle g, const NumericExecutable &ex,
                                   std::span<const Buffer> inputs,
                                   std::span<const std::uint64_t> args = {}) {
    return withTask(c, [&](auto &r) {
      using R = decltype(r);
      using B = For<R, cl::Buffer, vk::Buffer>;
      std::vector<B> native;
      native.reserve(inputs.size());
      for (auto &b : inputs)
        native.push_back(std::get<B>(b.value));
      return r.executeNumericProgram(
          c, g, std::get<For<R, cl::NumericExecutable, vk::NumericExecutable>>(ex.value), native,
          args);
    });
  }
  cl::Stats stats() {
    if (!active_.load(std::memory_order_acquire))
      return {};
    return std::visit(
        [](auto &r) {
          auto s = r->stats();
          return cl::Stats{s.bytes, s.inFlight, s.argumentBytes, s.initializedDevices};
        },
        *selected());
  }

private:
  explicit Runtime(Options options) : options_(options) {}
  template <class Caller, class F> TaskHandle withTask(Caller &caller, F &&fn) {
    const auto &result = selected();
    auto error = Error::Unavailable;
    if (result) {
      try {
        return std::visit([&](auto &r) { return fn(*r); }, *result);
      } catch (const std::bad_variant_access &) {
        error = Error::Residency;
      } catch (const std::bad_alloc &) {
        error = Error::Allocation;
      }
    } else
      error = result.error();
    if constexpr (std::is_same_v<Caller, Task>)
      caller.fault(static_cast<std::int64_t>(error));
    return {};
  }
  Options options_;
  Selection<Context, Error> selection_;
  std::atomic<bool> active_{};
  std::filesystem::path directory_;
  std::span<const Module> modules_;
};
} // namespace kalandra::runtime::bundle
