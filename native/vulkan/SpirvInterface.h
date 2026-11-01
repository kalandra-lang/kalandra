#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>
namespace kalandra::runtime::vulkan {
// Reflection for the compiler-owned flat storage-buffer ABI. This is not a
// general SPIR-V validator or an arbitrary user shader loading interface.
struct SpirvInterface {
  std::string entry;
  std::uint32_t localSize = 0, buffers = 0, parameterWords = 0;
  bool int8 = false, int16 = false, int64 = false, float64 = false;
  bool storage8 = false, storage16 = false;
  bool preserveFloat32 = false;
  bool flattenedGrid = false;
  bool groupOnlyGrid = false;
};
std::expected<SpirvInterface, std::string> reflectSpirv(std::span<const std::uint32_t> words);
} // namespace kalandra::runtime::vulkan
