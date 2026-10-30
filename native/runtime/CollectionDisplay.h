#pragma once
#include "Runtime.h"
#include "kalandra/CollectionABI.h"
#include "kalandra/NumericType.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace kalandra::runtime {
struct DisplayCollection {
  AllocationLease backing;
  std::span<const std::byte> data;
  std::uint64_t count;
};
// The CLI consumes owned, contiguous output. Validate the complete range even
// when the display limit means only a prefix will be printed.
inline std::optional<DisplayCollection> retainForDisplay(const Invocation &context,
                                                         const cli::CollectionDescriptor &value,
                                                         std::size_t rank, NumericType element,
                                                         bool array) noexcept {
  if (!context.usable() || rank > cli::maximumCollectionRank || (array && rank != 1) ||
      !(array ? numericCollectionStorageAvailable(element, NumericTarget::Cpu)
              : numericTensorElementwiseAvailable(element, NumericTarget::Cpu)) ||
      value.offset < 0)
    return {};
  bool empty = false;
  for (std::size_t axis = 0; axis < rank; ++axis) {
    if (value.dimensions[axis] < 0)
      return {};
    empty |= value.dimensions[axis] == 0;
  }
  if (empty)
    return DisplayCollection{{}, {}, 0};
  std::uint64_t count = 1;
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const auto extent = std::uint64_t(value.dimensions[axis]);
    if (count > std::uint64_t(INT64_MAX) / extent)
      return {};
    count *= extent;
  }
  auto bytes = checkedNumericBytes(element, count, INT64_MAX);
  if (!bytes)
    return {};
  std::uint64_t stride = 1;
  for (std::size_t axis = rank; axis-- > 0;) {
    if (value.dimensions[rank + axis] < 0 || std::uint64_t(value.dimensions[rank + axis]) != stride)
      return {};
    stride *= std::uint64_t(value.dimensions[axis]);
  }
  auto lease = context.lease(value.allocated);
  if (!lease)
    return {};
  const auto allocation = lease->bytes();
  const auto base = reinterpret_cast<std::uintptr_t>(value.allocated);
  const auto aligned = reinterpret_cast<std::uintptr_t>(value.aligned);
  const auto width = describeNumeric(element)->bytes();
  if (aligned < base || aligned % width || aligned - base > allocation.size())
    return {};
  auto offset = aligned - base;
  if (std::uint64_t(value.offset) > (allocation.size() - offset) / width)
    return {};
  offset += std::uint64_t(value.offset) * width;
  if (*bytes > allocation.size() - offset)
    return {};
  return DisplayCollection{std::move(*lease), allocation.subspan(offset, *bytes), count};
}
inline std::optional<DisplayCollection>
retainBoolForDisplay(Invocation &context, const cli::CollectionDescriptor &value) noexcept {
  kalandra_bool_array_validate_v1(&context, &value);
  if (!context.usable())
    return {};
  const auto count = std::uint64_t(value.dimensions[0]);
  if (!count)
    return DisplayCollection{{}, {}, 0};
  auto lease = context.lease(value.allocated);
  if (!lease)
    return {};
  auto data = lease->bytes().first(count);
  return DisplayCollection{std::move(*lease), data, count};
}
inline bool printBoolArray(FILE *stream, const DisplayCollection &value,
                           std::uint64_t limit = 100) noexcept {
  if (std::fputs("[", stream) < 0)
    return false;
  const auto shown = std::min(value.count, limit);
  for (std::uint64_t i = 0; i < shown; ++i) {
    if (i && std::fputs(", ", stream) < 0)
      return false;
    if (std::fputs(value.data[i] == std::byte{1} ? "true" : "false", stream) < 0)
      return false;
  }
  if (shown < value.count && std::fputs(shown ? ", ..." : "...", stream) < 0)
    return false;
  return std::fputs("]\n", stream) >= 0;
}
template <class T> inline T displayCell(std::span<const std::byte> data, std::uint64_t index) {
  T cell;
  std::memcpy(&cell, data.data() + index * sizeof(T), sizeof(T));
  return cell;
}
inline bool printCollection(FILE *stream, const DisplayCollection &value,
                            std::span<const std::int64_t> shape, NumericType element, bool array,
                            std::uint64_t limit = 100) noexcept {
  if (std::fputs(array ? "[" : "tensor([", stream) < 0)
    return false;
  const auto shown = std::min(value.count, limit);
  for (std::uint64_t i = 0; i < shown; ++i) {
    if (i && std::fputs(", ", stream) < 0)
      return false;
    int status = -1;
    switch (element) {
    case NumericType::Int8:
      status = std::fprintf(stream, "%d", int(displayCell<std::int8_t>(value.data, i)));
      break;
    case NumericType::UInt8:
      status = std::fprintf(stream, "%u", unsigned(displayCell<std::uint8_t>(value.data, i)));
      break;
    case NumericType::Int16:
      status = std::fprintf(stream, "%d", int(displayCell<std::int16_t>(value.data, i)));
      break;
    case NumericType::UInt16:
      status = std::fprintf(stream, "%u", unsigned(displayCell<std::uint16_t>(value.data, i)));
      break;
    case NumericType::Int32:
      status = std::fprintf(stream, "%d", int(displayCell<std::int32_t>(value.data, i)));
      break;
    case NumericType::UInt32:
      status = std::fprintf(stream, "%u", unsigned(displayCell<std::uint32_t>(value.data, i)));
      break;
    case NumericType::Int64:
      status = std::fprintf(stream, "%lld",
                            static_cast<long long>(displayCell<std::int64_t>(value.data, i)));
      break;
    case NumericType::UInt64:
      status =
          std::fprintf(stream, "%llu",
                       static_cast<unsigned long long>(displayCell<std::uint64_t>(value.data, i)));
      break;
    case NumericType::Float32:
      status = std::fprintf(stream, "%.9g", double(displayCell<float>(value.data, i)));
      break;
    case NumericType::Float64:
      status = std::fprintf(stream, "%.17g", displayCell<double>(value.data, i));
      break;
    default:
      return false;
    }
    if (status < 0)
      return false;
  }
  if (shown < value.count && std::fputs(shown ? ", ..." : "...", stream) < 0)
    return false;
  if (std::fputs("]", stream) < 0)
    return false;
  if (!array) {
    if (std::fputs(", [", stream) < 0)
      return false;
    for (std::size_t axis = 0; axis < shape.size(); ++axis)
      if (std::fprintf(stream, "%s%lld", axis ? ", " : "", static_cast<long long>(shape[axis])) < 0)
        return false;
    if (std::fputs("])", stream) < 0)
      return false;
  }
  if (shown < value.count &&
      std::fprintf(stream, " (%llu elements)", static_cast<unsigned long long>(value.count)) < 0)
    return false;
  return std::fputs("\n", stream) >= 0 && !std::ferror(stream);
}
} // namespace kalandra::runtime
