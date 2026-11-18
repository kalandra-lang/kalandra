#pragma once
#include "../include/kalandra/GpuTextWire.h"
#include "StringArray.h"
#include "Tensor.h"
#include <bit>
#include <cstring>

namespace kalandra::runtime {
using GpuTextWords = CpuTensor<NumericType::UInt32, 1>;

namespace detail {
template <class ReadRow>
GpuTextWords packGpuText(Invocation &context, std::uint64_t rows, ReadRow readRow,
                         std::uint32_t maxRowBytes) noexcept {
  if (context.poll() || !context.usable())
    return {};
  if (!GpuTextWire::wordCount(rows, 0)) {
    context.fault(2);
    return {};
  }
  std::uint64_t bytes = 0;
  for (std::uint64_t row = 0; row < rows; ++row) {
    const auto value = readRow(row);
    if (value.byteLength > maxRowBytes) {
      if (!context.poll())
        context.fault(2);
      return {};
    }
    if (!validateString(context, value))
      return {};
    const auto length = static_cast<std::uint64_t>(value.byteLength);
    if (length > UINT32_MAX - bytes) {
      context.fault(2);
      return {};
    }
    bytes += length;
  }
  const auto words = GpuTextWire::wordCount(rows, bytes);
  if (!words) {
    context.fault(2);
    return {};
  }
  auto *buffer = static_cast<std::uint32_t *>(context.allocate(*words * sizeof(std::uint32_t)));
  if (!buffer)
    return {};
  buffer[0] = static_cast<std::uint32_t>(rows);
  buffer[1] = static_cast<std::uint32_t>(bytes);
  buffer[GpuTextWire::HeaderWords] = 0;
  auto *payload = buffer + GpuTextWire::payloadStart(buffer[0]);
  if (bytes % 4)
    payload[bytes / 4] = 0;
  std::uint64_t position = 0;
  for (std::uint64_t row = 0; row < rows; ++row) {
    if (context.poll()) {
      context.release(buffer);
      return {};
    }
    const auto value = readRow(row);
    const auto length = static_cast<std::uint64_t>(value.byteLength);
    for (std::uint64_t i = 0; i < length;) {
      if (context.poll()) {
        context.release(buffer);
        return {};
      }
      const auto chunk = std::min<std::uint64_t>(1024, length - i);
      if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(reinterpret_cast<char *>(payload) + position, value.data + i, chunk);
      } else {
        for (std::uint64_t j = 0; j < chunk; ++j) {
          const auto offset = position + j;
          if (offset % 4 == 0)
            payload[offset / 4] = 0;
          const auto byte = static_cast<unsigned char>(value.data[i + j]);
          payload[offset / 4] |= std::uint32_t(byte) << (8 * (offset % 4));
        }
      }
      i += chunk;
      position += chunk;
    }
    buffer[GpuTextWire::HeaderWords + row + 1] = static_cast<std::uint32_t>(position);
  }
  if (context.poll()) {
    context.release(buffer);
    return {};
  }
  return {buffer, buffer, 0, {static_cast<std::int64_t>(*words)}, {1}};
}
} // namespace detail

inline GpuTextWords packGpuText(Invocation &context, const StringDescriptor &text,
                                std::uint32_t maxRowBytes = UINT32_MAX) noexcept {
  return detail::packGpuText(context, 1, [&](std::uint64_t) { return text; }, maxRowBytes);
}

inline GpuTextWords packGpuText(Invocation &context, const StringArrayDescriptor &array,
                                std::uint32_t maxRowBytes = UINT32_MAX) noexcept {
  kalandra_record_array_validate_v1(&context, &array, sizeof(StringDescriptor),
                                    alignof(StringDescriptor));
  if (context.poll() || !context.usable())
    return {};
  const auto lease = array.allocated ? context.lease(array.allocated) : std::nullopt;
  if (array.size && !lease) {
    context.fault(3);
    return {};
  }
  return detail::packGpuText(
      context, static_cast<std::uint64_t>(array.size),
      [&](std::uint64_t row) {
        StringDescriptor value;
        std::memcpy(&value, lease->bytes().data() + row * sizeof(value), sizeof(value));
        return value;
      },
      maxRowBytes);
}

// Decode a single variable-length device result. The caller keeps a lease on
// the downloaded words through the call. No output descriptor is published
// until the header, UTF-8 and invocation state have all been checked.
inline bool unpackGpuText(Invocation &context, std::span<const std::uint32_t> words,
                          StringDescriptor &result) noexcept {
  result = {};
  if (context.poll() || !context.usable())
    return false;
  const auto invalid = [&] {
    context.fault(3);
    return false;
  };
  constexpr auto start = GpuTextWire::payloadStart(1);
  if (words.size() < start || words[0] != 1 ||
      GpuTextWire::wordCount(1, words[1]) != words.size() || words[2] != 0 || words[3] != words[1])
    return invalid();
  const auto bytes = words[1];
  if (!bytes)
    return true;
  if (bytes % 4 && (words.back() >> (8 * (bytes % 4))))
    return invalid();
  auto *buffer = static_cast<char *>(context.allocateRecord(bytes));
  if (!buffer)
    return false;
  const auto *payload = words.data() + start;
  for (std::uint64_t i = 0; i < bytes;) {
    if (context.poll()) {
      context.release(buffer);
      return false;
    }
    const auto chunk = std::min<std::uint64_t>(1024, bytes - i);
    if constexpr (std::endian::native == std::endian::little) {
      std::memcpy(buffer + i, reinterpret_cast<const char *>(payload) + i, chunk);
    } else {
      for (std::uint64_t j = 0; j < chunk; ++j) {
        const auto offset = i + j;
        const auto byte = static_cast<unsigned char>(payload[offset / 4] >> (8 * (offset % 4)));
        std::memcpy(buffer + offset, &byte, 1);
      }
    }
    i += chunk;
  }
  const StringDescriptor value{buffer, buffer, bytes};
  if (!validateString(context, value)) {
    context.release(buffer);
    return false;
  }
  result = value;
  return true;
}
} // namespace kalandra::runtime
