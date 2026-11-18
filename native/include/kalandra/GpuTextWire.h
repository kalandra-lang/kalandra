#pragma once
#include <cstdint>
#include <optional>

namespace kalandra {
// A GPU text batch is [row count, byte count, offsets..., packed UTF-8...].
// Offsets are byte positions relative to the payload, including a terminal
// offset. A String uses one row, even when the text is empty. Each word holds
// up to four bytes in increasing order from its least significant byte.
struct GpuTextWire {
  static constexpr std::uint64_t HeaderWords = 2;
  static constexpr std::uint64_t MaxWords = UINT32_MAX;
  // Cooperative shaders cannot yield at checkpoints during a text scan.
  static constexpr std::uint32_t MaxCooperativeRowBytes = 16 * 1024;

  static constexpr std::uint64_t payloadStart(std::uint32_t rows) noexcept {
    return HeaderWords + rows + 1;
  }
  static constexpr std::optional<std::uint64_t> wordCount(std::uint64_t rows,
                                                          std::uint64_t bytes) noexcept {
    if (rows > UINT32_MAX || bytes > UINT32_MAX)
      return {};
    const auto words = payloadStart(static_cast<std::uint32_t>(rows)) + (bytes + 3) / 4;
    return words <= MaxWords ? std::optional{words} : std::nullopt;
  }
};
} // namespace kalandra
