#pragma once
#include "../include/kalandra/NumericType.h"
#include <bit>
#include <cstddef>
#include <cstdint>
namespace kalandra::runtime {
struct DeviceHeapBlockABI {
  // Mark state uses the low bits and grey links fit within 34 bits.
  static constexpr std::uint64_t ReadOnly = UINT64_C(1) << 63;
};
// Reserved only in frames whose terminal arena binding has ExportRole.
struct DeviceArrayExportABI {
  static constexpr std::size_t Root = 24, Data = 25, Length = 26, Origin = 27, Layout = 28,
                               Cells = 29;
};
// Payload carried as a fresh UInt64 array by the existing export protocol.
struct DeviceAggregateEnvelopeABI {
  static constexpr std::uint64_t Magic = UINT64_C(0x4b414c4147470001);
  static constexpr unsigned HeaderWords = 2, LeafWords = 3;
  static constexpr unsigned Scalar = 0, Length = 0, Origin = 1, Offset = 2;
};
struct DeviceArrayResultMetadata {
  std::uint64_t layout = 0, length = 0, origin = 0;
  bool operator==(const DeviceArrayResultMetadata &) const = default;
};
// Private, versioned transport cells. No native pointer or task-layout identity
// is a wire value. Source capture IDs preserve input aliases at result decoding.
struct DeviceArrayWireABI {
  static_assert(std::endian::native == std::endian::little,
                "GPU array wire requires a little-endian host");
  static constexpr std::uint64_t Magic = UINT64_C(0x4b414c4152520001);
  static constexpr std::uint64_t ResultMagic = UINT64_C(0x4b414c4152520002);
  static constexpr std::uint64_t MaxWords = UINT32_MAX;
  static constexpr std::size_t HeaderWords = 4, PageWords = 64 * 1024 / 8;
  static constexpr std::size_t Version = 0, Layout = 1, Length = 2, Origin = 3;
  static constexpr std::uint8_t InputRole = 2, ExportRole = 3;
  // Local arena handles occupy at most 32 bits. Keep the external tag in
  // the immediately following bit, leaving the sign bit out of handle dispatch.
  static constexpr std::uint64_t ExternalHandle = UINT64_C(1) << 32;
  static_assert(ExternalHandle > MaxWords);
  static constexpr std::uint64_t layout(NumericType type, bool boolean) {
    return boolean ? 11 : static_cast<unsigned>(type) + 1;
  }
};
} // namespace kalandra::runtime
