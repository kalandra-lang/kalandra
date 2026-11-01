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

namespace kalandra::runtime::vulkan {
// Private bring-up API. Not yet a source-language or task backend interface.
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
  std::uint32_t vendor = 0, device = 0, apiVersion = 0;
  bool discrete = false, integrated = false;
  std::array<std::uint8_t, 16> uuid{};
};
struct ByteBudget;
struct Options {
  bool validation = true;
  std::uint32_t deviceIndex = 0;
  std::uint64_t maxBytes = 64 * 1024 * 1024;
  std::shared_ptr<ByteBudget> budget{};
  // A retained enumeration selects by stable device identity, not a second
  // enumeration's possibly changed index. When present this replaces the index.
  std::optional<std::array<std::uint8_t, 16>> deviceUuid{};
};
struct State;
struct Storage;
struct KernelState;
struct HostCharge;
class Kernel final {
public:
  explicit operator bool() const noexcept { return bool(state_); }
  std::uint32_t inputCount() const noexcept;
  std::uint32_t localSize() const noexcept;
  std::uint32_t parameterWords() const noexcept;
  std::string_view entryName() const noexcept;

private:
  std::shared_ptr<KernelState> state_;
  friend class Runtime;
};
class Buffer final {
public:
  Buffer() = default;
  std::uint64_t bytes() const noexcept;
  explicit operator bool() const noexcept { return bool(storage_); }

private:
  std::shared_ptr<Storage> storage_;
  explicit Buffer(std::shared_ptr<Storage> storage) : storage_(std::move(storage)) {}
  friend class Runtime;
};
struct KernelResult {
  Buffer output;
  std::vector<std::uint32_t> parameters;
};
class Runtime final {
public:
  static std::expected<std::vector<DeviceInfo>, Error> enumerate(bool validation = true);
  static std::expected<std::unique_ptr<Runtime>, Error> create(Options = {});
  static std::expected<std::shared_ptr<ByteBudget>, Error> createByteBudget(std::uint64_t maximum);
  static std::expected<std::shared_ptr<const HostCharge>, Error>
  reserveBytes(const std::shared_ptr<ByteBudget> &, std::uint64_t);
  static std::uint64_t reservedBytes(const std::shared_ptr<ByteBudget> &) noexcept;
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  DeviceInfo device() const;
  bool owns(const Buffer &) const noexcept;
  bool owns(const Kernel &) const noexcept;
  // Host snapshots and retained CPU allocations share the device-byte ledger.
  std::expected<std::shared_ptr<const HostCharge>, Error> reserveHostBytes(std::uint64_t);
  bool owns(const std::shared_ptr<const HostCharge> &) const noexcept;
  std::expected<Buffer, Error> upload(std::span<const float>);
  std::expected<Buffer, Error> uploadBytes(std::span<const std::byte>);
  // Invocation-private scratch, zero initialized without an uncharged host copy.
  std::expected<Buffer, Error> blankBytes(std::uint64_t);
  std::expected<std::vector<std::byte>, Error> downloadBytes(const Buffer &);
  std::expected<void, Error> readBytes(const Buffer &, std::span<std::byte>);
  // Compiler-owned SPIR-V only: flat set-0 storage buffers, fixed 1D workgroups.
  std::expected<Kernel, Error> loadKernel(const std::filesystem::path &);
  std::expected<KernelResult, Error> execute(const Kernel &, std::span<const Buffer> inputs,
                                             std::span<const std::uint32_t> parameters,
                                             std::uint64_t outputBytes, std::uint32_t groups);
  std::expected<std::vector<float>, Error> download(const Buffer &);
  std::expected<Buffer, Error> add(const Buffer &, const Buffer &);
  // right is explicitly stored as [columns, contraction], not [contraction, columns].
  // Result is [rows, columns]. No hidden CPU numerical implementation/fallback.
  std::expected<Buffer, Error> matmulRightTransposed(const Buffer &left, const Buffer &right,
                                                     std::uint32_t rows, std::uint32_t columns,
                                                     std::uint32_t contraction);
  std::uint64_t retainedBytes() const;
  std::uint64_t validationErrors() const;

private:
  explicit Runtime(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};
} // namespace kalandra::runtime::vulkan
