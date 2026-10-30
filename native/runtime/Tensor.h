#pragma once
#include "NumericStorage.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>
namespace kalandra::runtime {
// Trusted native adapter view, separate from the MLIR memref ABI. Metadata
// spans must remain valid through validation; successful retention snapshots them.
struct CpuTensorView {
  const void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0;
  std::span<const std::int64_t> sizes, strides;
  NumericType element = static_cast<NumericType>(-1);
};
template <NumericType Element, std::size_t Rank> struct CpuTensor {
  static_assert(Rank <= 8);
  NumericValue<Element> *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0;
  std::array<std::int64_t, Rank> sizes{}, strides{};
  CpuTensorView view() const noexcept {
    return {allocated, aligned, offset, sizes, strides, Element};
  }
};
// Rank zero has no metadata arrays in the LLVM ABI (std::array<T,0> has size 1).
template <NumericType Element> struct CpuTensor<Element, 0> {
  NumericValue<Element> *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0;
  CpuTensorView view() const noexcept { return {allocated, aligned, offset, {}, {}, Element}; }
};
template <std::size_t Rank> using Float32Tensor = CpuTensor<NumericType::Float32, Rank>;
enum class CpuTensorError { Allocation = 2, Invalid = 3 };
inline std::optional<std::uint64_t> cpuTensorBytes(NumericType element,
                                                   std::span<const std::int64_t> shape) noexcept {
  if (!describeNumeric(element) || shape.size() > 8)
    return {};
  bool empty = false;
  for (auto dimension : shape) {
    if (dimension < 0)
      return {};
    empty |= dimension == 0;
  }
  if (empty)
    return 0;
  std::uint64_t count = 1;
  for (auto dimension : shape) {
    if (std::uint64_t(dimension) > UINT64_MAX / count)
      return {};
    count *= std::uint64_t(dimension);
  }
  return checkedNumericBytes(element, count, std::numeric_limits<std::size_t>::max());
}
class RetainedCpuTensor;
inline std::optional<RetainedCpuTensor> retainCpuTensor(const Invocation &, CpuTensorView) noexcept;
class RetainedCpuTensor final {
public:
  RetainedCpuTensor(RetainedCpuTensor &&) noexcept = default;
  RetainedCpuTensor &operator=(RetainedCpuTensor &&) noexcept = default;
  NumericType element() const noexcept { return element_; }
  std::span<const std::int64_t> shape() const noexcept { return {shape_.data(), rank_}; }
  std::uint64_t bytes() const noexcept { return bytes_; }
  std::uint64_t offsetBytes() const noexcept { return offset_; }
  std::uint64_t retainedBytes() const noexcept { return allocation_.bytes().size(); }
  std::span<const std::byte> data() const noexcept {
    const auto bytes = allocation_.bytes();
    if (offset_ > bytes.size() || bytes_ > bytes.size() - offset_)
      return {};
    return bytes.subspan(offset_, bytes_);
  }
  AllocationLease releaseAllocation() && noexcept { return std::move(allocation_); }

private:
  RetainedCpuTensor() = default;
  AllocationLease allocation_;
  NumericType element_ = NumericType::Float32;
  std::array<std::int64_t, 8> shape_{};
  std::size_t rank_ = 0;
  std::uint64_t bytes_ = 0, offset_ = 0;
  friend std::optional<RetainedCpuTensor> retainCpuTensor(const Invocation &,
                                                          CpuTensorView) noexcept;
};
inline std::optional<RetainedCpuTensor> retainCpuTensor(const Invocation &context,
                                                        CpuTensorView view) noexcept {
  const auto bytes = cpuTensorBytes(view.element, view.sizes);
  const auto alignment = numericAlignment(view.element);
  if (!context.usable() || !bytes || !alignment || view.sizes.size() != view.strides.size() ||
      view.offset < 0)
    return {};
  RetainedCpuTensor result;
  result.element_ = view.element;
  result.rank_ = view.sizes.size();
  std::copy(view.sizes.begin(), view.sizes.end(), result.shape_.begin());
  result.bytes_ = *bytes;
  // Empty descriptors do not dereference or retain their addresses.
  if (!*bytes)
    return result;
  std::uint64_t stride = 1;
  for (std::size_t axis = view.sizes.size(); axis-- > 0;) {
    if (view.strides[axis] < 0 || std::uint64_t(view.strides[axis]) != stride ||
        std::uint64_t(view.sizes[axis]) > UINT64_MAX / stride)
      return {};
    stride *= std::uint64_t(view.sizes[axis]);
  }
  auto lease = context.lease(view.allocated);
  if (!lease)
    return {};
  const auto allocation = lease->bytes();
  const auto base = reinterpret_cast<std::uintptr_t>(view.allocated);
  const auto aligned = reinterpret_cast<std::uintptr_t>(view.aligned);
  if (aligned < base || aligned % *alignment || aligned - base > allocation.size())
    return {};
  auto displacement = aligned - base;
  const auto width = describeNumeric(view.element)->bytes();
  if (std::uint64_t(view.offset) > (allocation.size() - displacement) / width)
    return {};
  displacement += std::uint64_t(view.offset) * width;
  if (*bytes > allocation.size() - displacement)
    return {};
  result.offset_ = displacement;
  result.allocation_ = std::move(*lease);
  return result;
}
inline std::expected<void *, CpuTensorError>
copyCpuTensorData(Invocation &context, NumericType element, std::span<const std::byte> data,
                  std::span<const std::int64_t> shape, std::span<std::int64_t> sizes,
                  std::span<std::int64_t> strides) noexcept {
  const auto bytes = cpuTensorBytes(element, shape);
  if (!context.usable() || !bytes || *bytes != data.size() || shape.size() != sizes.size() ||
      sizes.size() != strides.size())
    return std::unexpected(CpuTensorError::Invalid);
  std::array<std::int64_t, 8> canonical{};
  std::uint64_t stride = 1;
  for (std::size_t axis = shape.size(); axis-- > 0;) {
    if (stride > INT64_MAX || (shape[axis] && stride > UINT64_MAX / std::uint64_t(shape[axis])))
      return std::unexpected(CpuTensorError::Invalid);
    canonical[axis] = stride;
    stride *= std::uint64_t(shape[axis]);
  }
  auto *output = context.allocate(*bytes);
  if (!output)
    return std::unexpected(CpuTensorError::Allocation);
  if (*bytes)
    std::memcpy(output, data.data(), *bytes);
  std::copy(shape.begin(), shape.end(), sizes.begin());
  std::copy_n(canonical.begin(), shape.size(), strides.begin());
  return output;
}
template <NumericType Element, std::size_t Rank>
std::expected<CpuTensor<Element, Rank>, CpuTensorError>
copyCpuTensor(Invocation &context, NumericType sourceElement, std::span<const std::byte> data,
              std::span<const std::int64_t> shape) noexcept {
  if (sourceElement != Element)
    return std::unexpected(CpuTensorError::Invalid);
  CpuTensor<Element, Rank> result;
  std::span<std::int64_t> sizes, strides;
  if constexpr (Rank != 0) {
    sizes = result.sizes;
    strides = result.strides;
  }
  auto copied = copyCpuTensorData(context, Element, data, shape, sizes, strides);
  if (!copied)
    return std::unexpected(copied.error());
  result.allocated = result.aligned = static_cast<NumericValue<Element> *>(*copied);
  return result;
}
static_assert(std::is_standard_layout_v<Float32Tensor<1>>);
static_assert(sizeof(void *) == 8 && sizeof(Float32Tensor<0>) == 24);
static_assert(sizeof(Float32Tensor<1>) == 40 && sizeof(Float32Tensor<2>) == 56);
static_assert(offsetof(Float32Tensor<1>, sizes) == 24 && offsetof(Float32Tensor<1>, strides) == 32);
} // namespace kalandra::runtime
