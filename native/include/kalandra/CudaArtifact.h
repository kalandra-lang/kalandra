#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kalandra {
// Private, versioned compiler/runtime envelope. PTX comments keep the file
// directly consumable by NVIDIA tools. The checksum catches corruption, not
// hostile code: compiler-owned GPU modules are not a public kernel sandbox.
struct CudaBufferSpec {
  std::uint32_t bits = 0;
  std::int64_t extent = -1;
  bool operator==(const CudaBufferSpec &) const = default;
};
struct CudaArtifact {
  std::string entry;
  std::uint32_t chip = 0, threads = 0, policy = 0;
  bool nativeFloat64 = false;
  std::vector<CudaBufferSpec> buffers;
  std::string ptx;
  std::uint32_t tile = 0;
  std::uint32_t catalog = 0; // V3: bounded full-FP32 split-K catalog, no public ABI change.
  bool operator==(const CudaArtifact &) const = default;
};
inline std::uint64_t cudaArtifactChecksum(std::string_view bytes) noexcept {
  std::uint64_t value = 14695981039346656037ULL;
  for (unsigned char byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}
inline std::string encodeCudaArtifact(const CudaArtifact &value) {
  if (value.tile && value.catalog)
    return {};
  std::string header = std::string(value.catalog ? "// KALANDRA_CUDA_V3 "
                                   : value.tile  ? "// KALANDRA_CUDA_V2 "
                                                 : "// KALANDRA_CUDA_V1 ") +
                       std::to_string(value.chip) + " " + std::to_string(value.threads) + " " +
                       std::to_string(value.policy) + " " + std::to_string(value.nativeFloat64) +
                       " " + std::to_string(value.buffers.size()) + " " + value.entry + " " +
                       std::to_string(value.ptx.size());
  if (value.tile || value.catalog)
    header += " " + std::to_string(value.catalog ? value.catalog : value.tile);
  std::string tail;
  for (auto buffer : value.buffers)
    tail += "// BUFFER " + std::to_string(buffer.bits) + " " + std::to_string(buffer.extent) + "\n";
  tail += "// PTX\n";
  tail += value.ptx;
  const auto checksum = cudaArtifactChecksum(header + "\n" + tail);
  return header + " " + std::to_string(checksum) + "\n" + tail;
}
inline std::optional<CudaArtifact> decodeCudaArtifact(std::string_view input) {
  const auto original = input;
  if (input.size() > 16 * 1024 * 1024 || input.find('\0') != input.npos)
    return {};
  const auto line = [&]() -> std::optional<std::string_view> {
    auto end = input.find('\n');
    if (end == input.npos || end > 1024)
      return {};
    auto value = input.substr(0, end);
    input.remove_prefix(end + 1);
    return value;
  };
  const auto tokens = [](std::string_view text) {
    std::vector<std::string_view> result;
    while (!text.empty()) {
      auto end = text.find(' ');
      result.push_back(text.substr(0, end));
      if (end == text.npos)
        break;
      text.remove_prefix(end + 1);
    }
    return result;
  };
  const auto number = []<class T>(std::string_view text, T &value) {
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && error == std::errc{} && end == text.data() + text.size();
  };
  auto first = line();
  if (!first)
    return {};
  auto fields = tokens(*first);
  if (fields.size() < 2 || fields[0] != "//")
    return {};
  const bool tiled = fields[1] == "KALANDRA_CUDA_V2";
  const bool catalog = fields[1] == "KALANDRA_CUDA_V3";
  if ((!tiled && !catalog && fields[1] != "KALANDRA_CUDA_V1") ||
      fields.size() != (tiled || catalog ? 11u : 10u))
    return {};
  CudaArtifact result;
  std::uint32_t count = 0, native = 0;
  std::uint64_t length = 0, checksum = 0;
  if (!number(fields[2], result.chip) || !number(fields[3], result.threads) ||
      !number(fields[4], result.policy) || !number(fields[5], native) ||
      !number(fields[6], count) || !number(fields[8], length) || !number(fields.back(), checksum) ||
      (result.chip != 80 && result.chip != 120) || !result.threads || result.threads > 1024 ||
      result.policy > 2 || native > 1 || (result.policy && native) || count < 2 || count > 64 ||
      fields[7].empty() || fields[7].size() > 192)
    return {};
  for (char c : fields[7])
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
      return {};
  if (tiled && (!number(fields[9], result.tile) || (result.tile != 64 && result.tile != 128)))
    return {};
  if (catalog && (!number(fields[9], result.catalog) || result.catalog != 1))
    return {};
  result.entry = fields[7];
  result.nativeFloat64 = native;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto next = line();
    if (!next)
      return {};
    auto values = tokens(*next);
    CudaBufferSpec buffer;
    if (values.size() != 4 || values[0] != "//" || values[1] != "BUFFER" ||
        !number(values[2], buffer.bits) || !number(values[3], buffer.extent) ||
        (buffer.bits != 8 && buffer.bits != 16 && buffer.bits != 32 && buffer.bits != 64) ||
        buffer.extent < -1 || buffer.extent > UINT32_MAX)
      return {};
    result.buffers.push_back(buffer);
  }
  auto marker = line();
  if (!marker || *marker != "// PTX" || input.size() != length ||
      result.buffers.back().bits != 32 || result.buffers.back().extent <= 0 ||
      result.buffers.back().extent > 65536)
    return {};
  if (tiled || catalog) {
    if (result.policy || result.nativeFloat64 || result.threads != 128 ||
        result.buffers.size() != 4 ||
        result.entry != (catalog ? "kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1"
                                 : "kalandra_numeric_v1_matmul_float32_tile" +
                                       std::to_string(result.tile) + "_cuda_desc_v1") ||
        result.buffers.back().extent != 4)
      return {};
    for (std::size_t i = 0; i < result.buffers.size(); ++i)
      if (result.buffers[i].bits != 32 || (i < 3 && result.buffers[i].extent != -1))
        return {};
  }
  result.ptx = input;
  if (encodeCudaArtifact(result) != original)
    return {};
  return result;
}
} // namespace kalandra
