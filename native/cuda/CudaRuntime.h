#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace kalandra::runtime::cuda {
enum class Error : std::int64_t {
  Shape = 1,
  Allocation = 2,
  Invalid = 3,
  Arithmetic = 4,
  Bounds = 5,
  Budget = 6,
  Residency = 7,
  Unavailable = 104,
  Device = 105,
  Unsupported = 106
};
struct DeviceInfo {
  std::string name;
  std::uint32_t vendor = 0x10de, device = 0, apiVersion = 0;
  bool discrete = true, integrated = false;
  std::array<std::uint8_t, 16> uuid{};
};
struct ByteBudget;
struct HostCharge;
// Private deterministic controls. Launch fails before submission; a wait
// failure is reported only after real device completion. No GPU reset/hang.
enum class FailurePoint {
  None,
  Allocate,
  Upload,
  ModuleLoad,
  Launch,
  WaitAfterCompletion,
  Readback,
  ScratchAllocate,
  ScratchDevice,
  ReduceLaunch
};
struct State;
struct Storage;
struct KernelState;
struct Options {
  bool validation = false;
  unsigned deviceIndex = 0;
  std::uint64_t maxBytes = 64 * 1024 * 1024;
  std::shared_ptr<ByteBudget> budget{};
  std::optional<std::array<std::uint8_t, 16>> deviceUuid{};
  bool enableFloat64 = true, enableTiledMatmul = true;
  unsigned maxModules = 64;
};
class Buffer {
  std::shared_ptr<Storage> storage_;
  explicit Buffer(std::shared_ptr<Storage> value) : storage_(std::move(value)) {}
  friend class Runtime;

public:
  Buffer() = default;
  std::uint64_t bytes() const noexcept;
  explicit operator bool() const noexcept { return bool(storage_); }
};
class Kernel {
  std::shared_ptr<KernelState> state_, fixed_;
  friend class Runtime;

public:
  Kernel() = default;
  std::uint32_t inputCount() const noexcept;
  std::uint32_t localSize() const noexcept;
  std::uint32_t parameterWords() const noexcept;
  std::string_view entryName() const noexcept;
  bool tiledMatmul() const noexcept;
  bool rightTransposedMatmul() const noexcept { return false; }
  explicit operator bool() const noexcept { return bool(state_); }
};
struct KernelContract {
  std::string entry;
  std::uint64_t inputs = 0, parameterWords = 0;
};
struct DispatchStats {
  std::uint64_t nativeMatmuls = 0, fixedMatmuls = 0;
  std::uint64_t splitMatmuls = 0, scratchFreeMatmuls = 0, bandedMatmuls = 0;
};
struct KernelResult {
  Buffer output;
  std::vector<std::uint32_t> parameters;
};
// Private synchronous bring-up service. Completion precedes resource retirement;
// the task/foreign-service layer will own asynchronous language integration.
class Runtime {
  std::shared_ptr<State> state_;
  explicit Runtime(std::shared_ptr<State> value) : state_(std::move(value)) {}
  std::expected<void, Error> executeGrid(const Kernel &, std::span<const Buffer>, unsigned,
                                         unsigned);

  std::expected<Buffer, Error> allocateBytes(std::uint64_t, bool initialize, bool optional);
  std::expected<void, Error> executeCatalog(const Kernel &, std::span<const Buffer>,
                                            const Buffer &scratch, const Buffer &bandParameters,
                                            bool wide, unsigned m, unsigned n, unsigned k,
                                            unsigned bandRows);

public:
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  ~Runtime() = default;
  static std::expected<std::unique_ptr<Runtime>, Error> create(Options = {});
  static std::expected<std::vector<DeviceInfo>, Error> enumerate(bool validation = false);
  static std::expected<std::shared_ptr<ByteBudget>, Error> createByteBudget(std::uint64_t);
  static std::expected<std::shared_ptr<const HostCharge>, Error>
  reserveBytes(const std::shared_ptr<ByteBudget> &, std::uint64_t);
  static std::uint64_t reservedBytes(const std::shared_ptr<ByteBudget> &) noexcept;
  std::expected<std::shared_ptr<const HostCharge>, Error> reserveHostBytes(std::uint64_t);
  bool owns(const Buffer &) const noexcept;
  bool owns(const Kernel &) const noexcept;
  bool owns(const std::shared_ptr<const HostCharge> &) const noexcept;
  DeviceInfo device() const;
  std::expected<Buffer, Error> uploadBytes(std::span<const std::byte>);
  std::expected<Buffer, Error> blankBytes(std::uint64_t);
  std::expected<std::vector<std::byte>, Error> downloadBytes(const Buffer &);
  std::expected<Kernel, Error> loadKernel(const std::filesystem::path &,
                                          const KernelContract * = nullptr);
  std::expected<Kernel, Error> loadMatmulKernel(const std::filesystem::path &);
  std::expected<Kernel, Error> loadBuiltinTransposeFloat32() {
    return std::unexpected(Error::Unsupported);
  }
  std::expected<void, Error> readBytes(const Buffer &, std::span<std::byte>);
  std::expected<KernelResult, Error> execute(const Kernel &, std::span<const Buffer>,
                                             std::span<const std::uint32_t>,
                                             std::uint64_t outputBytes, std::uint32_t groups);
  // Buffers include output and parameters in compiler descriptor order. Never
  // launches arbitrary user PTX: admission requires the private artifact codec.
  std::expected<void, Error> execute(const Kernel &, std::span<const Buffer>, unsigned groups);
  std::expected<Buffer, Error> upload(std::span<const float>);
  std::expected<std::vector<float>, Error> download(const Buffer &);
  bool healthy() const;
  DispatchStats dispatchStats() const noexcept;
  std::uint64_t retainedBytes() const noexcept;
  void injectFailure(FailurePoint) noexcept;
};
} // namespace kalandra::runtime::cuda
