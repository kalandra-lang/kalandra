#include "Cuda.h"
#include "kalandra/CudaArtifact.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda.h>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <mutex>

namespace kalandra::runtime::cuda {
namespace {
bool driverSucceeded(CUresult status, const char *operation) {
  if (status != CUDA_SUCCESS && std::getenv("KALANDRA_CUDA_DIAGNOSTICS"))
    std::fprintf(stderr, "CUDA %s failed: %d\n", operation, int(status));
  return status == CUDA_SUCCESS;
}
struct Driver {
  void *library = nullptr;
#define CUDA_FUNCTION(name, type) type name = nullptr
  using Init = CUresult (*)(unsigned);
  using Version = CUresult (*)(int *);
  using DeviceCount = CUresult (*)(int *);
  using DeviceName = CUresult (*)(char *, int, CUdevice);
  using Uuid = CUresult (*)(CUuuid *, CUdevice);
  using DeviceGet = CUresult (*)(CUdevice *, int);
  using Attribute = CUresult (*)(int *, CUdevice_attribute, CUdevice);
  using Create = CUresult (*)(CUcontext *, unsigned, CUdevice);
  using Context = CUresult (*)(CUcontext);
  using Pop = CUresult (*)(CUcontext *);
  using Sync = CUresult (*)();
  using Granularity = CUresult (*)(size_t *, const CUmemAllocationProp *,
                                   CUmemAllocationGranularity_flags);
  using Alloc = CUresult (*)(CUdeviceptr *, size_t);
  using Free = CUresult (*)(CUdeviceptr);
  using HtoD = CUresult (*)(CUdeviceptr, const void *, size_t);
  using DtoH = CUresult (*)(void *, CUdeviceptr, size_t);
  using Zero = CUresult (*)(CUdeviceptr, unsigned char, size_t);
  using Load = CUresult (*)(CUmodule *, const void *, unsigned, CUjit_option *, void **);
  using Unload = CUresult (*)(CUmodule);
  using Function = CUresult (*)(CUfunction *, CUmodule, const char *);
  using Count = CUresult (*)(CUfunction, size_t *);
  using Parameter = CUresult (*)(CUfunction, size_t, size_t *, size_t *);
  using Launch = CUresult (*)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, CUstream, void **, void **);
  CUDA_FUNCTION(version, Version);
  CUDA_FUNCTION(countDevices, DeviceCount);
  CUDA_FUNCTION(name, DeviceName);
  CUDA_FUNCTION(uuid, Uuid);
  CUDA_FUNCTION(init, Init);
  CUDA_FUNCTION(deviceGet, DeviceGet);
  CUDA_FUNCTION(attribute, Attribute);
  CUDA_FUNCTION(create, Create);
  CUDA_FUNCTION(push, Context);
  CUDA_FUNCTION(pop, Pop);
  CUDA_FUNCTION(destroy, Context);
  CUDA_FUNCTION(sync, Sync);
  CUDA_FUNCTION(alloc, Alloc);
  CUDA_FUNCTION(free, Free);
  CUDA_FUNCTION(htod, HtoD);
  CUDA_FUNCTION(dtoh, DtoH);
  CUDA_FUNCTION(zero, Zero);
  CUDA_FUNCTION(load, Load);
  CUDA_FUNCTION(unload, Unload);
  CUDA_FUNCTION(function, Function);
  CUDA_FUNCTION(count, Count);
  CUDA_FUNCTION(parameter, Parameter);
  CUDA_FUNCTION(launch, Launch);
  CUDA_FUNCTION(granularity, Granularity);
#undef CUDA_FUNCTION
  ~Driver() {
    if (library)
      dlclose(library);
  }
  bool open() {
    library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
      return false;
#define SYMBOL(field, name)                                                                        \
  field = reinterpret_cast<decltype(field)>(dlsym(library, name));                                 \
  if (!field)                                                                                      \
  return false
    SYMBOL(version, "cuDriverGetVersion");
    SYMBOL(countDevices, "cuDeviceGetCount");
    SYMBOL(name, "cuDeviceGetName");
    SYMBOL(uuid, "cuDeviceGetUuid_v2");
    SYMBOL(init, "cuInit");
    SYMBOL(deviceGet, "cuDeviceGet");
    SYMBOL(attribute, "cuDeviceGetAttribute");
    SYMBOL(create, "cuCtxCreate_v2");
    SYMBOL(push, "cuCtxPushCurrent_v2");
    SYMBOL(pop, "cuCtxPopCurrent_v2");
    SYMBOL(destroy, "cuCtxDestroy_v2");
    SYMBOL(sync, "cuCtxSynchronize");
    SYMBOL(alloc, "cuMemAlloc_v2");
    SYMBOL(free, "cuMemFree_v2");
    SYMBOL(htod, "cuMemcpyHtoD_v2");
    SYMBOL(dtoh, "cuMemcpyDtoH_v2");
    SYMBOL(zero, "cuMemsetD8_v2");
    SYMBOL(load, "cuModuleLoadDataEx");
    SYMBOL(unload, "cuModuleUnload");
    SYMBOL(function, "cuModuleGetFunction");
    SYMBOL(count, "cuFuncGetParamCount");
    SYMBOL(parameter, "cuFuncGetParamInfo");
    SYMBOL(launch, "cuLaunchKernel");
    SYMBOL(granularity, "cuMemGetAllocationGranularity");
#undef SYMBOL
    return driverSucceeded(init(0), "cuInit");
  }
};
} // namespace
struct ByteBudget {
  std::atomic<std::uint64_t> retained{0};
  std::uint64_t maximum;
  explicit ByteBudget(std::uint64_t size) : maximum(size) {}
  bool reserve(std::uint64_t size) {
    auto old = retained.load();
    do {
      if (size > maximum || old > maximum - size)
        return false;
    } while (!retained.compare_exchange_weak(old, old + size));
    return true;
  }
};
struct HostCharge {
  std::shared_ptr<ByteBudget> budget;
  std::uint64_t size = 0;
  ~HostCharge() {
    if (budget)
      budget->retained.fetch_sub(size);
  }
};
struct State {
  Driver driver;
  CUcontext context = nullptr;
  std::mutex mutex, moduleMutex;
  std::vector<std::weak_ptr<KernelState>> moduleCache;
  std::shared_ptr<ByteBudget> budget;
  std::uint64_t granularity = 0, deferredBytes = 0;
  DeviceInfo info;
  bool enableFloat64 = true, enableTiled = true;
  std::atomic<std::uint64_t> nativeMatmuls{0}, fixedMatmuls{0};
  int chip = 0, maxThreads = 0, maxGrid = 0, maxGridY = 0;
  std::atomic<unsigned> modules{0};
  unsigned maxModules = 0;
  bool poisoned = false;
  std::atomic<FailurePoint> failure{FailurePoint::None};
  bool fail(FailurePoint point) {
    return failure.compare_exchange_strong(point, FailurePoint::None);
  }
  ~State() {
    if (context && driver.destroy(context) != CUDA_SUCCESS)
      return;
    if (budget)
      budget->retained.fetch_sub(deferredBytes);
  }
};
namespace {
struct Current {
  State &state;
  bool entered;
  explicit Current(State &s) : state(s), entered(s.driver.push(s.context) == CUDA_SUCCESS) {}
  ~Current() {
    if (entered) {
      CUcontext previous{};
      if (state.driver.pop(&previous) != CUDA_SUCCESS)
        state.poisoned = true;
    }
  }
};
} // namespace
struct Storage {
  std::shared_ptr<State> owner;
  CUdeviceptr pointer = 0;
  std::uint64_t size = 0, charge = 0;
  ~Storage() {
    if (!pointer)
      return;
    std::lock_guard lock(owner->mutex);
    Current current(*owner);
    // Failed frees remain charged until context destruction reclaims memory.
    if (current.entered && owner->driver.free(pointer) == CUDA_SUCCESS)
      owner->budget->retained.fetch_sub(charge);
    else {
      owner->poisoned = true;
      owner->deferredBytes += charge;
    }
  }
};
struct KernelState {
  std::shared_ptr<State> owner;
  CUmodule module{};
  CUfunction function{};
  CudaArtifact artifact;
  bool charged = false;
  ~KernelState() {
    if (module) {
      std::lock_guard lock(owner->mutex);
      Current current(*owner);
      if (!current.entered || owner->driver.unload(module) != CUDA_SUCCESS) {
        // A failed unload remains charged until context destruction. Releasing
        // the slot before acquiring the queue lock would allow new admissions
        // to outrun pending cleanup and exceed the actual module bound.
        owner->poisoned = true;
        return;
      }
    }
    if (charged)
      owner->modules.fetch_sub(1);
  }
};
std::uint64_t Buffer::bytes() const noexcept { return storage_ ? storage_->size : 0; }
void Runtime::injectFailure(FailurePoint point) noexcept { state_->failure.store(point); }
std::uint64_t Runtime::retainedBytes() const noexcept { return state_->budget->retained.load(); }
std::expected<std::unique_ptr<Runtime>, Error> Runtime::create(Options options) {
  if (!options.maxBytes || !options.maxModules ||
      options.deviceIndex > unsigned(std::numeric_limits<int>::max()))
    return std::unexpected(Error::Invalid);
  auto state = std::make_shared<State>();
  state->maxModules = options.maxModules;
  state->enableFloat64 = options.enableFloat64;
  state->enableTiled = options.enableTiledMatmul;
  if (options.budget)
    state->budget = options.budget;
  else
    state->budget = std::make_shared<ByteBudget>(options.maxBytes);
  if (!state->driver.open())
    return std::unexpected(Error::Unavailable);
  CUdevice device{};
  auto devices = enumerate();
  if (!devices)
    return std::unexpected(devices.error());
  if (options.deviceUuid) {
    auto found = std::find_if(devices->begin(), devices->end(),
                              [&](const auto &d) { return d.uuid == *options.deviceUuid; });
    if (found == devices->end())
      return std::unexpected(Error::Unavailable);
    options.deviceIndex = found - devices->begin();
  }
  if (options.deviceIndex >= devices->size())
    return std::unexpected(Error::Unavailable);
  state->info = (*devices)[options.deviceIndex];
  int major{}, minor{};
  auto &d = state->driver;
  if (!driverSucceeded(d.deviceGet(&device, int(options.deviceIndex)), "cuDeviceGet") ||
      !driverSucceeded(d.attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device),
                       "compute major") ||
      !driverSucceeded(d.attribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device),
                       "compute minor") ||
      !driverSucceeded(
          d.attribute(&state->maxThreads, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, device),
          "max threads") ||
      !driverSucceeded(d.attribute(&state->maxGrid, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, device),
                       "max grid"))
    return std::unexpected(Error::Unavailable);
  if (!driverSucceeded(d.attribute(&state->maxGridY, CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y, device),
                       "max grid y"))
    return std::unexpected(Error::Unavailable);
  CUuuid selectedUuid{};
  if (!driverSucceeded(d.uuid(&selectedUuid, device), "selected UUID") ||
      std::memcmp(selectedUuid.bytes, state->info.uuid.data(), 16) != 0)
    return std::unexpected(Error::Unavailable);
  state->chip = major * 10 + minor;
  CUmemAllocationProp property{};
  property.type = CU_MEM_ALLOCATION_TYPE_PINNED;
  property.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  property.location.id = device;
  size_t granularity{};
  if (!driverSucceeded(d.granularity(&granularity, &property, CU_MEM_ALLOC_GRANULARITY_MINIMUM),
                       "allocation granularity") ||
      !granularity)
    return std::unexpected(Error::Unsupported);
  state->granularity = granularity;
  if (!driverSucceeded(d.create(&state->context, CU_CTX_SCHED_BLOCKING_SYNC, device),
                       "cuCtxCreate"))
    return std::unexpected(Error::Device);
  CUcontext previous{};
  if (!driverSucceeded(d.pop(&previous), "cuCtxPopCurrent"))
    return std::unexpected(Error::Device);
  return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
}
std::expected<Buffer, Error> Runtime::blankBytes(std::uint64_t size) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->poisoned)
      return std::unexpected(Error::Device);
  }

  // Charge conservatively at the device allocation granularity. Request the
  // logical extent from CUDA so Compute Sanitizer can detect accesses beyond
  // a tensor instead of treating quota padding as valid application memory.
  const auto granularity = state_->granularity;
  if (!size) {
    auto storage = std::make_shared<Storage>();
    storage->owner = state_;
    return Buffer(std::move(storage)); // Owned empty tensor, no CUDA allocation.
  }
  if (size > SIZE_MAX - (granularity - 1))
    return std::unexpected(Error::Invalid);
  auto charge = (size + granularity - 1) / granularity * granularity;
  auto storage = std::make_shared<Storage>();
  storage->owner = state_;
  if (!state_->budget->reserve(charge))
    return std::unexpected(Error::Budget);
  std::unique_lock lock(state_->mutex);
  Current current(*state_);
  CUdeviceptr pointer{};
  if (!current.entered || state_->poisoned || state_->fail(FailurePoint::Allocate) ||
      state_->driver.alloc(&pointer, size) != CUDA_SUCCESS) {
    state_->budget->retained.fetch_sub(charge);
    return std::unexpected(Error::Allocation);
  }
  const auto initialized = state_->driver.zero(pointer, 0, size);
  const auto completed = state_->driver.sync();
  if (initialized != CUDA_SUCCESS || completed != CUDA_SUCCESS) {
    if (state_->driver.free(pointer) == CUDA_SUCCESS)
      state_->budget->retained.fetch_sub(charge);
    else
      state_->deferredBytes += charge;
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  storage->pointer = pointer;
  storage->size = size;
  storage->charge = charge;
  return Buffer(std::move(storage));
}
std::expected<Buffer, Error> Runtime::uploadBytes(std::span<const std::byte> data) {
  auto buffer = blankBytes(data.size());
  if (!buffer)
    return std::unexpected(buffer.error());
  if (data.empty())
    return std::move(*buffer);
  std::lock_guard lock(state_->mutex);
  Current current(*state_);
  if (!current.entered || state_->poisoned || state_->fail(FailurePoint::Upload) ||
      state_->driver.htod(buffer->storage_->pointer, data.data(), data.size()) != CUDA_SUCCESS)
    return std::unexpected(Error::Device);
  return std::move(*buffer);
}
std::expected<std::vector<std::byte>, Error> Runtime::downloadBytes(const Buffer &buffer) {
  if (!buffer.storage_ || buffer.storage_->owner != state_)
    return std::unexpected(Error::Invalid);
  std::vector<std::byte> result(buffer.bytes());
  if (result.empty())
    return result;
  std::lock_guard lock(state_->mutex);
  Current current(*state_);
  if (!current.entered || state_->poisoned || state_->fail(FailurePoint::Readback) ||
      state_->driver.dtoh(result.data(), buffer.storage_->pointer, result.size()) != CUDA_SUCCESS)
    return std::unexpected(Error::Device);
  return result;
}
std::expected<Kernel, Error> Runtime::loadKernel(const std::filesystem::path &path,
                                                 const KernelContract *contract) {
  std::lock_guard moduleLock(state_->moduleMutex);
  {
    std::lock_guard lock(state_->mutex);
    if (state_->poisoned)
      return std::unexpected(Error::Device);
  }
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || file.tellg() > 16 * 1024 * 1024)
    return std::unexpected(Error::Invalid);
  std::string text(std::size_t(file.tellg()), '\0');
  file.seekg(0);
  if (!file.read(text.data(), text.size()))
    return std::unexpected(Error::Invalid);
  auto artifact = decodeCudaArtifact(text);
  if (!artifact)
    return std::unexpected(Error::Invalid);
  if (contract &&
      (contract->entry != artifact->entry || contract->inputs != artifact->buffers.size() - 2 ||
       contract->parameterWords != std::uint64_t(artifact->buffers.back().extent)))
    return std::unexpected(Error::Invalid);
  if (artifact->nativeFloat64 && !state_->enableFloat64)
    return std::unexpected(Error::Unsupported);
  if (artifact->chip > unsigned(state_->chip) || artifact->threads > unsigned(state_->maxThreads))
    return std::unexpected(Error::Unsupported);
  std::erase_if(state_->moduleCache, [](auto &value) { return value.expired(); });
  for (auto &weak : state_->moduleCache)
    if (auto cached = weak.lock(); cached && cached->artifact == *artifact) {
      Kernel result;
      result.state_ = std::move(cached);
      return result;
    }
  auto kernel = std::make_shared<KernelState>();
  kernel->owner = state_;
  kernel->artifact = std::move(*artifact);
  auto live = state_->modules.load();
  do {
    if (live >= state_->maxModules)
      return std::unexpected(Error::Budget);
  } while (!state_->modules.compare_exchange_weak(live, live + 1));
  kernel->charged = true;
  // This scope ends before module RAII runs on an admission failure.
  bool valid = false;
  {
    std::lock_guard lock(state_->mutex);
    Current current(*state_);
    if (current.entered && !state_->poisoned && !state_->fail(FailurePoint::ModuleLoad) &&
        state_->driver.load(&kernel->module, text.c_str(), 0, nullptr, nullptr) == CUDA_SUCCESS &&
        state_->driver.function(&kernel->function, kernel->module,
                                kernel->artifact.entry.c_str()) == CUDA_SUCCESS) {
      size_t count{};
      valid = state_->driver.count(kernel->function, &count) == CUDA_SUCCESS &&
              count == kernel->artifact.buffers.size() * 5;
      for (size_t i = 0; valid && i < count; ++i) {
        size_t offset{}, size{};
        valid = state_->driver.parameter(kernel->function, i, &offset, &size) == CUDA_SUCCESS &&
                offset == i * 8 && size == 8;
      }
    }
  }
  if (!valid)
    return std::unexpected(Error::Invalid);
  state_->moduleCache.push_back(kernel);
  Kernel result;
  result.state_ = std::move(kernel);
  return result;
}
std::expected<void, Error> Runtime::execute(const Kernel &kernel, std::span<const Buffer> buffers,
                                            unsigned groups) {
  if (kernel.tiledMatmul())
    return std::unexpected(Error::Invalid);
  return executeGrid(kernel, buffers, groups, 1);
}
std::expected<void, Error> Runtime::executeGrid(const Kernel &kernel,
                                                std::span<const Buffer> buffers, unsigned groups,
                                                unsigned rows) {
  if (!kernel.state_ || kernel.state_->owner != state_ || !groups ||
      groups > unsigned(state_->maxGrid) || !rows || rows > unsigned(state_->maxGridY) ||
      buffers.size() != kernel.state_->artifact.buffers.size())
    return std::unexpected(Error::Invalid);
  std::vector<std::uint64_t> descriptors;
  descriptors.reserve(buffers.size() * 5);
  for (size_t i = 0; i < buffers.size(); ++i) {
    auto &buffer = buffers[i];
    auto spec = kernel.state_->artifact.buffers[i];
    auto width = spec.bits / 8;
    if (!buffer.storage_ || buffer.storage_->owner != state_ || buffer.bytes() % width ||
        (spec.extent >= 0 && buffer.bytes() / width != std::uint64_t(spec.extent)))
      return std::unexpected(Error::Invalid);
    descriptors.insert(descriptors.end(), {buffer.storage_->pointer, buffer.storage_->pointer, 0,
                                           buffer.bytes() / width, 1});
  }
  std::vector<void *> parameters;
  parameters.reserve(descriptors.size());
  for (auto &word : descriptors)
    parameters.push_back(&word);
  std::lock_guard lock(state_->mutex);
  Current current(*state_);
  if (!current.entered || state_->poisoned)
    return std::unexpected(Error::Device);
  if (state_->fail(FailurePoint::Launch)) {
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  auto launched = state_->driver.launch(kernel.state_->function, groups, rows, 1,
                                        kernel.state_->artifact.threads, 1, 1, 0, nullptr,
                                        parameters.data(), nullptr);
  // Always settle after entering the launch API, including errors. No capture
  // or device allocation can retire while submitted work may still use it.
  auto settled = state_->driver.sync();
  if (launched != CUDA_SUCCESS || settled != CUDA_SUCCESS ||
      state_->fail(FailurePoint::WaitAfterCompletion)) {
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  return {};
}
} // namespace kalandra::runtime::cuda

namespace kalandra::runtime::cuda {
bool Kernel::tiledMatmul() const noexcept { return state_ && state_->artifact.tile != 0; }
DispatchStats Runtime::dispatchStats() const noexcept {
  return {state_->nativeMatmuls.load(), state_->fixedMatmuls.load()};
}
std::expected<Kernel, Error> Runtime::loadMatmulKernel(const std::filesystem::path &path) {
  auto native = loadKernel(path);
  if (!native)
    return std::unexpected(native.error());
  if (!native->tiledMatmul())
    return native;
  auto alternate = path;
  alternate += ".fixed-tree.ptx";
  const KernelContract contract{"kalandra_numeric_v1_matmul_float32_cuda_desc_v1", 2, 4};
  auto fixed = loadKernel(alternate, &contract);
  if (!fixed)
    return std::unexpected(fixed.error());
  const std::vector<CudaBufferSpec> fixedAbi{{32, -1}, {32, -1}, {32, -1}, {32, 4}};
  if (fixed->tiledMatmul() || fixed->state_->artifact.policy != 0 ||
      fixed->state_->artifact.nativeFloat64 || fixed->state_->artifact.buffers != fixedAbi)
    return std::unexpected(Error::Invalid);
  if (!state_->enableTiled)
    return fixed;
  native->fixed_ = fixed->state_;
  return native;
}
std::uint32_t Kernel::inputCount() const noexcept {
  return state_ ? state_->artifact.buffers.size() - 2 : 0;
}
std::uint32_t Kernel::localSize() const noexcept { return state_ ? state_->artifact.threads : 0; }
std::uint32_t Kernel::parameterWords() const noexcept {
  return state_ ? state_->artifact.buffers.back().extent : 0;
}
std::string_view Kernel::entryName() const noexcept {
  return state_ ? std::string_view(state_->artifact.entry) : std::string_view{};
}
DeviceInfo Runtime::device() const { return state_->info; }
bool Runtime::owns(const Buffer &b) const noexcept {
  return b.storage_ && b.storage_->owner == state_;
}
bool Runtime::owns(const Kernel &k) const noexcept { return k.state_ && k.state_->owner == state_; }
bool Runtime::owns(const std::shared_ptr<const HostCharge> &c) const noexcept {
  return c && c->budget == state_->budget;
}
std::expected<std::vector<DeviceInfo>, Error> Runtime::enumerate(bool) {
  Driver d;
  if (!d.open())
    return std::unexpected(Error::Unavailable);
  int count{}, version{};
  if (!driverSucceeded(d.version(&version), "driver version"))
    return std::unexpected(Error::Device);
  if (d.countDevices(&count) != CUDA_SUCCESS || count < 0 || count > 4096)
    return std::unexpected(Error::Device);
  std::vector<DeviceInfo> result;
  for (int i = 0; i < count; ++i) {
    CUdevice device{};
    CUuuid uuid{};
    char name[256]{};
    if (d.deviceGet(&device, i) != CUDA_SUCCESS ||
        d.name(name, sizeof(name), device) != CUDA_SUCCESS || d.uuid(&uuid, device) != CUDA_SUCCESS)
      return std::unexpected(Error::Device);
    int integrated{};
    if (!driverSucceeded(d.attribute(&integrated, CU_DEVICE_ATTRIBUTE_INTEGRATED, device),
                         "integrated device"))
      return std::unexpected(Error::Device);
    DeviceInfo info;
    info.name = name;
    info.device = i;
    info.apiVersion = version;
    info.integrated = integrated != 0;
    info.discrete = !info.integrated;
    std::memcpy(info.uuid.data(), uuid.bytes, 16);
    result.push_back(std::move(info));
  }
  return result;
}
std::expected<std::shared_ptr<ByteBudget>, Error> Runtime::createByteBudget(std::uint64_t maximum) {
  if (!maximum)
    return std::unexpected(Error::Invalid);
  return std::make_shared<ByteBudget>(maximum);
}
std::expected<std::shared_ptr<const HostCharge>, Error>
Runtime::reserveBytes(const std::shared_ptr<ByteBudget> &budget, std::uint64_t size) {
  if (!budget)
    return std::unexpected(Error::Invalid);
  auto charge = std::make_shared<HostCharge>();
  if (!budget->reserve(size))
    return std::unexpected(Error::Budget);
  charge->budget = budget;
  charge->size = size;
  return charge;
}
std::uint64_t Runtime::reservedBytes(const std::shared_ptr<ByteBudget> &budget) noexcept {
  return budget ? budget->retained.load() : 0;
}
std::expected<std::shared_ptr<const HostCharge>, Error>
Runtime::reserveHostBytes(std::uint64_t size) {
  return reserveBytes(state_->budget, size);
}
std::expected<void, Error> Runtime::readBytes(const Buffer &buffer, std::span<std::byte> output) {
  if (output.size() != buffer.bytes() || !owns(buffer))
    return std::unexpected(Error::Invalid);
  auto temporary = reserveHostBytes(buffer.bytes());
  if (!temporary)
    return std::unexpected(temporary.error());
  auto bytes = downloadBytes(buffer);
  if (!bytes)
    return std::unexpected(bytes.error());
  std::copy(bytes->begin(), bytes->end(), output.begin());
  return {};
}
std::expected<KernelResult, Error> Runtime::execute(const Kernel &kernel,
                                                    std::span<const Buffer> inputs,
                                                    std::span<const std::uint32_t> words,
                                                    std::uint64_t outputBytes,
                                                    std::uint32_t groups) {
  if (!owns(kernel) || kernel.inputCount() != inputs.size() ||
      kernel.parameterWords() != words.size())
    return std::unexpected(Error::Invalid);
  Kernel dispatched = kernel;
  unsigned gridRows = 1;
  bool nativeMatrix = false;
  if (kernel.tiledMatmul()) {
    if (inputs.size() != 2 || words.size() != 4 || !owns(inputs[0]) || !owns(inputs[1]))
      return std::unexpected(Error::Invalid);
    const std::uint64_t m = words[0], k = words[1], n = words[2];
    if (std::max({m * n, m * k, k * n}) > UINT32_MAX || outputBytes != m * n * 4 ||
        inputs[0].bytes() != m * k * 4 || inputs[1].bytes() != k * n * 4)
      return std::unexpected(Error::Invalid);
    const auto tile = kernel.state_->artifact.tile;
    nativeMatrix = m && n && k && !(m % tile) && !(n % tile) && !(k % 16) &&
                   std::max({m * n, m * k, k * n}) < std::uint64_t(INT32_MAX) &&
                   m / tile <= unsigned(state_->maxGridY) &&
                   n / tile <= unsigned(state_->maxGrid) &&
                   inputs[0].storage_ != inputs[1].storage_;
    if (nativeMatrix) {
      groups = n / tile;
      gridRows = m / tile;
    } else {
      if (!kernel.fixed_)
        return std::unexpected(Error::Unsupported);
      dispatched.state_ = kernel.fixed_;
      dispatched.fixed_.reset();
      const auto width = dispatched.localSize();
      groups = std::max(1u, std::uint32_t(m * n / width + unsigned(m * n % width != 0)));
    }
  }
  // Private transform ABIs have explicit logical shapes and scratch roles.
  // Validate them before any driver submission, including empty tensors.
  const auto entry = kernel.entryName();
  if (entry.starts_with("kalandra_tensor_transform_v1_")) {
    const bool transpose = entry.starts_with("kalandra_tensor_transform_v1_transpose_");
    const bool rms = entry.starts_with("kalandra_tensor_transform_v1_rmsNormRows_");
    if (words.size() != 6 ||
        inputs.size() != (transpose ? 1u
                          : rms     ? 3u
                                    : 2u) ||
        std::uint64_t(words[1]) * words[2] != words[0] || !owns(inputs[0]))
      return std::unexpected(Error::Invalid);
    unsigned width = 0;
    for (auto bits : {8u, 16u, 32u, 64u})
      if (entry.ends_with("int" + std::to_string(bits) + "_cuda_desc_v1") ||
          entry.ends_with("float" + std::to_string(bits) + "_cuda_desc_v1"))
        width = bits / 8;
    if (!width || (!transpose && width != 4) || outputBytes != std::uint64_t(words[0]) * width ||
        inputs[0].bytes() != outputBytes)
      return std::unexpected(Error::Invalid);
    if (!transpose) {
      const auto &scratch = inputs.back();
      if (!owns(scratch) || scratch.bytes() != outputBytes ||
          scratch.storage_ == inputs[0].storage_ ||
          (rms &&
           (words[4] > 1 || !owns(inputs[1]) ||
            (words[4] && (inputs[1].bytes() != 4 || scratch.storage_ == inputs[1].storage_)))))
        return std::unexpected(Error::Invalid);
    }
    // CUDA's byte-copy transpose is linear; Vulkan retains its imported
    // 32x32-tile dispatch. Empty row kernels still validate uniform epsilon.
    groups = !words[0] ? 1 : transpose ? words[0] / 256 + unsigned(words[0] % 256 != 0) : words[1];
  }
  auto output = blankBytes(outputBytes);
  if (!output)
    return std::unexpected(output.error());
  auto parameters = uploadBytes(std::as_bytes(words));
  if (!parameters)
    return std::unexpected(parameters.error());
  std::vector<Buffer> bindings(inputs.begin(), inputs.end());
  bindings.push_back(*output);
  bindings.push_back(*parameters);
  auto executed = executeGrid(dispatched, bindings, groups, gridRows);
  if (!executed)
    return std::unexpected(executed.error());
  if (kernel.entryName().starts_with("kalandra_numeric_v1_matmul_")) {
    if (std::getenv("KALANDRA_CUDA_DIAGNOSTICS"))
      std::fprintf(stderr, "CUDA matmul route=%s tile=%u shape=%u,%u,%u\n",
                   nativeMatrix ? "tiled" : "fixed-tree",
                   nativeMatrix ? kernel.state_->artifact.tile : 0, words[0], words[1], words[2]);
    if (nativeMatrix)
      state_->nativeMatmuls.fetch_add(1);
    else
      state_->fixedMatmuls.fetch_add(1);
  }
  KernelResult result{*output, std::vector<std::uint32_t>(words.size())};
  auto read = readBytes(*parameters, std::as_writable_bytes(std::span(result.parameters)));
  if (!read)
    return std::unexpected(read.error());
  return result;
}
} // namespace kalandra::runtime::cuda

namespace kalandra::runtime::cuda {
std::expected<Buffer, Error> Runtime::upload(std::span<const float> values) {
  return uploadBytes(std::as_bytes(values));
}
std::expected<std::vector<float>, Error> Runtime::download(const Buffer &buffer) {
  if (!owns(buffer) || buffer.bytes() % sizeof(float))
    return std::unexpected(Error::Invalid);
  std::vector<float> values(buffer.bytes() / sizeof(float));
  auto read = readBytes(buffer, std::as_writable_bytes(std::span(values)));
  if (!read)
    return std::unexpected(read.error());
  return values;
}
bool Runtime::healthy() const {
  std::lock_guard lock(state_->mutex);
  return !state_->poisoned;
}
} // namespace kalandra::runtime::cuda
