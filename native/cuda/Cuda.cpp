#include "Cuda.h"
#include "DriverLibrary.h"
#include "kalandra/CudaArtifact.h"
#include "kalandra/CudaMatmul.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda.h>
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
  DriverLibrary library;
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
  using FunctionAttribute = CUresult (*)(int *, CUfunction_attribute, CUfunction);
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
  CUDA_FUNCTION(functionAttribute, FunctionAttribute);
  CUDA_FUNCTION(parameter, Parameter);
  CUDA_FUNCTION(launch, Launch);
  CUDA_FUNCTION(granularity, Granularity);
#undef CUDA_FUNCTION
  bool open() {
    if (!library.open())
      return false;
#define SYMBOL(field, name)                                                                        \
  field = reinterpret_cast<decltype(field)>(library.symbol(name));                                 \
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
    SYMBOL(functionAttribute, "cuFuncGetAttribute");
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
  std::atomic<std::uint64_t> nativeMatmuls{0}, fixedMatmuls{0}, splitMatmuls{0},
      scratchFreeMatmuls{0}, bandedMatmuls{0};
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
  std::array<CUfunction, 3> catalog{}; // large, reducer, same-arithmetic no-scratch
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
  return allocateBytes(size, true, false);
}
std::expected<Buffer, Error> Runtime::allocateBytes(std::uint64_t size, bool initialize,
                                                    bool optional) {
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
  CUresult allocated = CUDA_ERROR_OUT_OF_MEMORY;
  if (!current.entered || state_->poisoned ||
      (optional && state_->fail(FailurePoint::ScratchDevice))) {
    state_->budget->retained.fetch_sub(charge);
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  if (!state_->fail(optional ? FailurePoint::ScratchAllocate : FailurePoint::Allocate))
    allocated = state_->driver.alloc(&pointer, size);
  if (allocated != CUDA_SUCCESS) {
    state_->budget->retained.fetch_sub(charge);
    if (allocated != CUDA_ERROR_OUT_OF_MEMORY) {
      state_->poisoned = true;
      return std::unexpected(Error::Device);
    }
    return std::unexpected(Error::Allocation);
  }
  // Private split-K scratch is completely overwritten before it is consumed.
  // Avoid a zero fill and completion fence solely for optional workspace.
  const auto initialized = initialize ? state_->driver.zero(pointer, 0, size) : CUDA_SUCCESS;
  const auto completed = initialize ? state_->driver.sync() : CUDA_SUCCESS;
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
  if (artifact->chip > unsigned(state_->chip) || artifact->threads > unsigned(state_->maxThreads) ||
      (artifact->catalog && state_->maxThreads < 256))
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
      const auto reflected = [&](CUfunction function) {
        size_t count{};
        if (state_->driver.count(function, &count) != CUDA_SUCCESS ||
            count != kernel->artifact.buffers.size() * 5)
          return false;
        for (size_t i = 0; i < count; ++i) {
          size_t offset{}, size{};
          if (state_->driver.parameter(function, i, &offset, &size) != CUDA_SUCCESS ||
              offset != i * 8 || size != 8)
            return false;
        }
        return true;
      };
      valid = reflected(kernel->function);
      if (kernel->artifact.catalog) {
        constexpr std::array suffixes{"_large", "_reduce", "_no_scratch"};
        for (unsigned i = 0; valid && i < suffixes.size(); ++i)
          valid = state_->driver.function(&kernel->catalog[i], kernel->module,
                                          (kernel->artifact.entry + suffixes[i]).c_str()) ==
                      CUDA_SUCCESS &&
                  reflected(kernel->catalog[i]);
        int localBytes = -1;
        valid = valid &&
                state_->driver.functionAttribute(&localBytes, CU_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES,
                                                 kernel->catalog[2]) == CUDA_SUCCESS &&
                localBytes == 0;
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
std::expected<void, Error>
Runtime::executeCatalog(const Kernel &kernel, std::span<const Buffer> buffers,
                        const Buffer &scratch, const Buffer &bandParameters, bool wide, unsigned m,
                        unsigned n, unsigned k, unsigned bandRows) {
  if (!owns(kernel) || !kernel.state_->artifact.catalog || buffers.size() != 4)
    return std::unexpected(Error::Invalid);
  std::array<std::uint64_t, 20> descriptor{};
  std::array<void *, 20> arguments{};
  for (unsigned i = 0; i < buffers.size(); ++i) {
    if (!owns(buffers[i]) || buffers[i].bytes() % 4)
      return std::unexpected(Error::Invalid);
    const auto base = i * 5;
    descriptor[base] = descriptor[base + 1] = buffers[i].storage_->pointer;
    descriptor[base + 3] = buffers[i].bytes() / 4;
    descriptor[base + 4] = 1;
  }
  if (scratch &&
      (!owns(scratch) || scratch.bytes() != std::uint64_t(bandRows) * n * 4 * cudaMatmulParts))
    return std::unexpected(Error::Invalid);
  const unsigned bands = scratch ? (m + bandRows - 1) / bandRows : 1;
  if (bandParameters &&
      (!owns(bandParameters) || bandParameters.bytes() != std::uint64_t(bands) * 16))
    return std::unexpected(Error::Invalid);
  if (scratch && bandRows != m && !bandParameters)
    return std::unexpected(Error::Invalid);
  for (unsigned i = 0; i < descriptor.size(); ++i)
    arguments[i] = &descriptor[i];
  const auto linearGroups = unsigned((std::uint64_t(m) * n + 255) / 256);
  std::lock_guard lock(state_->mutex);
  Current current(*state_);
  if (!current.entered || state_->poisoned)
    return std::unexpected(Error::Device);
  if (state_->fail(FailurePoint::Launch)) {
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  CUresult launched;
  if (!scratch) {
    launched = state_->driver.launch(kernel.state_->catalog[2], linearGroups, 1, 1, 256, 1, 1, 0,
                                     nullptr, arguments.data(), nullptr);
  } else {
    launched = CUDA_SUCCESS;
    const auto original = descriptor;
    const unsigned tm = wide ? 128 : 64, tn = wide ? 256 : 128;
    for (unsigned band = 0; band < bands && launched == CUDA_SUCCESS; ++band) {
      descriptor = original;
      const auto row = std::uint64_t(band) * bandRows;
      const auto rows = unsigned(std::min<std::uint64_t>(bandRows, m - row));
      descriptor[2] = row * k;
      if (bandParameters) {
        descriptor[15] = descriptor[16] = bandParameters.storage_->pointer;
        descriptor[17] = std::uint64_t(band) * 4;
      }
      descriptor[10] = descriptor[11] = scratch.storage_->pointer;
      descriptor[13] = scratch.bytes() / 4;
      launched =
          state_->driver.launch(wide ? kernel.state_->catalog[0] : kernel.state_->function,
                                (n + tn - 1) / tn, (rows + tm - 1) / tm, cudaMatmulParts,
                                wide ? 256 : 128, 1, 1, 0, nullptr, arguments.data(), nullptr);
      if (launched == CUDA_SUCCESS) {
        // CUDA copies arguments before returning. All bands reuse the scratch
        // on one stream, so each reduction completes before the next overwrite.
        descriptor[0] = descriptor[1] = scratch.storage_->pointer;
        descriptor[2] = 0;
        descriptor[3] = std::uint64_t(rows) * n * cudaMatmulParts;
        descriptor[10] = descriptor[11] = buffers[2].storage_->pointer;
        descriptor[12] = row * n;
        descriptor[13] = buffers[2].bytes() / 4;
        launched = state_->fail(FailurePoint::ReduceLaunch)
                       ? CUDA_ERROR_INVALID_VALUE
                       : state_->driver.launch(kernel.state_->catalog[1],
                                               unsigned((std::uint64_t(rows) * n + 255) / 256), 1,
                                               1, 256, 1, 1, 0, nullptr, arguments.data(), nullptr);
      }
    }
  }
  // Settle even after the first or second launch fails. Capture/scratch owners
  // cannot retire while submitted work may still access them. The language's
  // cancellation acknowledgement remains the completed source primitive.
  auto settled = state_->driver.sync();
  if (launched != CUDA_SUCCESS || settled != CUDA_SUCCESS ||
      state_->fail(FailurePoint::WaitAfterCompletion)) {
    state_->poisoned = true;
    return std::unexpected(Error::Device);
  }
  (scratch ? state_->splitMatmuls : state_->scratchFreeMatmuls).fetch_add(1);
  if (scratch && bands > 1)
    state_->bandedMatmuls.fetch_add(1);
  return {};
}
} // namespace kalandra::runtime::cuda

namespace kalandra::runtime::cuda {
bool Kernel::tiledMatmul() const noexcept {
  return state_ && (state_->artifact.tile != 0 || state_->artifact.catalog != 0);
}
DispatchStats Runtime::dispatchStats() const noexcept {
  return {state_->nativeMatmuls.load(), state_->fixedMatmuls.load(), state_->splitMatmuls.load(),
          state_->scratchFreeMatmuls.load(), state_->bandedMatmuls.load()};
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
    if (kernel.state_->artifact.catalog) {
      const bool wide = cudaMatmulWide(m, n, k);
      const unsigned tm = wide ? 128 : 64, tn = wide ? 256 : 128;
      nativeMatrix = cudaMatmulCatalogShape(m, n, k) &&
                     (m + tm - 1) / tm <= unsigned(state_->maxGridY) &&
                     (n + tn - 1) / tn <= unsigned(state_->maxGrid) &&
                     (m * n + 255) / 256 <= unsigned(state_->maxGrid) &&
                     inputs[0].storage_ != inputs[1].storage_;
    } else {
      nativeMatrix = m && n && k && !(m % tile) && !(n % tile) && !(k % 16) &&
                     std::max({m * n, m * k, k * n}) < std::uint64_t(INT32_MAX) &&
                     m / tile <= unsigned(state_->maxGridY) &&
                     n / tile <= unsigned(state_->maxGrid) &&
                     inputs[0].storage_ != inputs[1].storage_;
    }
    if (nativeMatrix) {
      if (tile) {
        groups = n / tile;
        gridRows = m / tile;
      }
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
  Buffer scratch, bandParameters;
  unsigned bandRows = words.empty() ? 0 : words[0];
  const bool catalog = nativeMatrix && kernel.state_->artifact.catalog;
  if (catalog) {
    auto made = allocateBytes(outputBytes * cudaMatmulParts, false, true);
    if (made)
      scratch = std::move(*made);
    else if (made.error() != Error::Budget && made.error() != Error::Allocation)
      return std::unexpected(made.error()); // Device errors never become fallback.
    else if (made.error() == Error::Budget && words[0] > 256) {
      // Bound workspace without raising the byte limit. Larger bands reduce
      // submission overhead; at most two fixed shapes are attempted on Budget.
      // Allocation/device errors never cause an unbounded retry loop.
      for (unsigned rows : {512u, 256u}) {
        if (rows >= words[0])
          continue;
        auto bounded =
            allocateBytes(std::uint64_t(rows) * words[2] * 4 * cudaMatmulParts, false, true);
        if (!bounded) {
          if (bounded.error() == Error::Budget)
            continue;
          if (bounded.error() == Error::Allocation)
            break;
          return std::unexpected(bounded.error());
        }
        const auto bands = (words[0] + rows - 1) / rows;
        const auto tableBytes = std::uint64_t(bands) * 16;
        auto host = reserveHostBytes(tableBytes);
        if (!host)
          continue;
        std::vector<std::uint32_t> table(std::size_t(bands) * 4);
        for (unsigned band = 0; band < bands; ++band) {
          table[band * 4] = std::min(rows, words[0] - band * rows);
          table[band * 4 + 1] = words[1];
          table[band * 4 + 2] = words[2];
          table[band * 4 + 3] = UINT32_MAX;
        }
        auto uploaded = uploadBytes(std::as_bytes(std::span(table)));
        if (!uploaded) {
          if (uploaded.error() == Error::Budget)
            continue;
          if (uploaded.error() == Error::Allocation)
            break;
          return std::unexpected(uploaded.error());
        }
        scratch = std::move(*bounded);
        bandParameters = std::move(*uploaded);
        bandRows = rows;
        break;
      }
    }
  }
  auto executed = catalog ? executeCatalog(dispatched, bindings, scratch, bandParameters,
                                           cudaMatmulWide(words[0], words[2], words[1]), words[0],
                                           words[2], words[1], bandRows)
                          : executeGrid(dispatched, bindings, groups, gridRows);
  if (!executed)
    return std::unexpected(executed.error());
  // Completion has settled both launches. Return optional workspace quota
  // before mandatory status readback reserves its host staging bytes.
  scratch = {};
  bandParameters = {};
  if (kernel.entryName().starts_with("kalandra_numeric_v1_matmul_")) {
    if (std::getenv("KALANDRA_CUDA_DIAGNOSTICS"))
      std::fprintf(stderr, "CUDA matmul route=%s tile=%u shape=%u,%u,%u\n",
                   nativeMatrix ? "tiled" : "fixed-tree",
                   nativeMatrix ? (catalog ? 64u : kernel.state_->artifact.tile) : 0, words[0],
                   words[1], words[2]);
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
