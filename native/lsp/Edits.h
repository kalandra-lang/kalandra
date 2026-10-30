#pragma once
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace kalandra::lsp {
struct Position {
  std::size_t line, character;
};
struct Range {
  Position start, end;
};
struct TextChange {
  std::optional<Range> range;
  std::string text;
  std::optional<std::size_t> rangeLength;
};
struct EditLimits {
  std::size_t maxBytes = 16 * 1024 * 1024 - 1;
  std::size_t maxChanges = 1024;
  std::size_t maxWorkBytes = 128 * 1024 * 1024;
};
// Shared cursor mapping for editor requests; invalid lines/surrogate halves return null.
[[nodiscard]] std::optional<std::size_t> textOffset(std::string_view text, Position position);
// Transactional, sequential UTF-16 edits. No mutation of caller storage on failure.
[[nodiscard]] std::expected<std::string, std::string>
applyChanges(std::string_view original, std::span<const TextChange> changes,
             EditLimits limits = {});
} // namespace kalandra::lsp
