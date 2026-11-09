#pragma once
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kalandra {
struct FormatPart {
  std::string text;
  bool argument = false;
};
struct FormatTemplateError {
  std::size_t offset;
  std::string_view message;
};
// The caller has already decoded the String literal and applied source limits.
// Each argument part consumes exactly one source argument, left to right.
inline std::expected<std::vector<FormatPart>, FormatTemplateError>
parseFormatTemplate(std::string_view text, std::size_t arguments) {
  std::vector<FormatPart> parts;
  std::string literal;
  std::size_t used = 0;
  const auto flush = [&] {
    if (!literal.empty()) {
      parts.push_back({std::move(literal), false});
      literal.clear();
    }
  };
  for (std::size_t i = 0; i < text.size();) {
    const char ch = text[i];
    if (ch != '{' && ch != '}') {
      literal += ch;
      ++i;
    } else if (i + 1 < text.size() && text[i + 1] == ch) {
      literal += ch;
      i += 2;
    } else if (ch == '{' && i + 1 < text.size() && text[i + 1] == '}') {
      if (used == arguments)
        return std::unexpected(FormatTemplateError{i, "format has more placeholders than arguments"});
      flush();
      parts.push_back({{}, true});
      ++used;
      i += 2;
    } else {
      return std::unexpected(FormatTemplateError{i, "format accepts {} placeholders and {{ or }} escapes"});
    }
  }
  if (used != arguments)
    return std::unexpected(FormatTemplateError{text.size(), "format has unused arguments"});
  flush();
  return parts;
}
} // namespace kalandra
