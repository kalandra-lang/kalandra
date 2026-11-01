#include "TensorBridge.h"
#include <algorithm>
#include <new>
namespace kalandra::runtime::vulkan {
struct HostTensorStorage {
  // Return quota after freeing/retiring the charged backing.
  std::shared_ptr<const HostCharge> charge;
  std::optional<RetainedCpuTensor> retained;
  std::vector<std::byte> owned;
  NumericType type;
  std::array<std::uint64_t, 8> dimensions{};
  std::uint8_t rank = 0;
  std::span<const std::byte> bytes() const {
    return retained ? retained->data() : std::span<const std::byte>(owned);
  }
};
std::uint64_t TensorValue::bytes() const noexcept {
  return data ? data.bytes() : host ? host->bytes().size() : 0;
}
namespace {
std::expected<std::uint64_t, Error> geometry(const TensorValue &value) {
  if (value.scalar || value.rank > 8)
    return std::unexpected(Error::Invalid);
  std::array<std::int64_t, 8> shape{};
  for (std::size_t i = 0; i < value.rank; ++i) {
    if (value.dimensions[i] > INT64_MAX)
      return std::unexpected(Error::Invalid);
    shape[i] = value.dimensions[i];
  }
  auto bytes = cpuTensorBytes(value.type, std::span(shape).first(value.rank));
  if (!bytes)
    return std::unexpected(Error::Invalid);
  return *bytes;
}
TensorValue metadata(NumericType type, std::span<const std::int64_t> shape) {
  TensorValue value;
  value.type = type;
  value.rank = shape.size();
  std::copy(shape.begin(), shape.end(), value.dimensions.begin());
  return value;
}
std::expected<std::shared_ptr<HostTensorStorage>, Error>
backing(Runtime &runtime, const TensorValue &value, std::uint64_t bytes) {
  auto charge = runtime.reserveHostBytes(bytes);
  if (!charge)
    return std::unexpected(charge.error());
  auto result = std::make_shared<HostTensorStorage>();
  result->charge = std::move(*charge);
  result->type = value.type;
  result->dimensions = value.dimensions;
  result->rank = value.rank;
  return result;
}
} // namespace
std::expected<TensorValue, Error> fromCpu(Runtime &runtime, const Invocation &context,
                                          CpuTensorView view) {
  auto retained = retainCpuTensor(context, view);
  if (!retained)
    return std::unexpected(Error::Invalid);
  try {
    auto value = metadata(retained->element(), retained->shape());
    auto storage = backing(runtime, value, retained->retainedBytes());
    if (!storage)
      return std::unexpected(storage.error());
    (*storage)->retained.emplace(std::move(*retained));
    value.host = std::move(*storage);
    return value;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<TensorValue, Error> hostTensor(Runtime &runtime, NumericType type,
                                             std::span<const std::byte> bytes,
                                             std::span<const std::int64_t> shape) {
  auto size = cpuTensorBytes(type, shape);
  if (!size || *size != bytes.size())
    return std::unexpected(Error::Invalid);
  try {
    auto value = metadata(type, shape);
    auto storage = backing(runtime, value, bytes.size());
    if (!storage)
      return std::unexpected(storage.error());
    (*storage)->owned.assign(bytes.begin(), bytes.end());
    value.host = std::move(*storage);
    return value;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::span<const std::byte>, Error> hostBytes(const TensorValue &value) {
  auto size = geometry(value);
  if (!size)
    return std::unexpected(size.error());
  if (value.data || !value.host)
    return std::unexpected(Error::Residency);
  const auto &storage = *value.host;
  if (value.type != storage.type || value.rank != storage.rank ||
      !std::equal(value.shape().begin(), value.shape().end(), storage.dimensions.begin()) ||
      *size != storage.bytes().size())
    return std::unexpected(Error::Invalid);
  return storage.bytes();
}
std::expected<void, Error> validateTransfer(Runtime &runtime, const TensorValue &value,
                                            bool upload) {
  auto size = geometry(value);
  if (!size)
    return std::unexpected(size.error());
  if (upload) {
    auto bytes = hostBytes(value);
    if (!bytes)
      return std::unexpected(bytes.error());
    if (!runtime.owns(value.host->charge))
      return std::unexpected(Error::Residency);
  } else {
    if (value.host || !runtime.owns(value.data))
      return std::unexpected(Error::Residency);
    if (*size != value.data.bytes())
      return std::unexpected(Error::Invalid);
  }
  return {};
}
std::expected<TensorValue, Error> transferTensor(Runtime &runtime, const TensorValue &input,
                                                 bool upload) {
  auto valid = validateTransfer(runtime, input, upload);
  if (!valid)
    return std::unexpected(valid.error());
  try {
    TensorValue value = input;
    if (upload) {
      auto result = runtime.uploadBytes(*hostBytes(input));
      if (!result)
        return std::unexpected(result.error());
      value.data = std::move(*result);
      value.host.reset();
    } else {
      auto storage = backing(runtime, input, input.data.bytes());
      if (!storage)
        return std::unexpected(storage.error());
      (*storage)->owned.resize(input.data.bytes());
      auto copied = runtime.readBytes(input.data, (*storage)->owned);
      if (!copied)
        return std::unexpected(copied.error());
      value.data = {};
      value.host = std::move(*storage);
    }
    return value;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
} // namespace kalandra::runtime::vulkan
