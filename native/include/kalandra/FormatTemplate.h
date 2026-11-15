#pragma once
#include "kalandra/FormatABI.h"
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kalandra {
struct FormatSpec {
  runtime::FormatPresentation presentation = runtime::FormatPresentation::Default;
  runtime::FormatAlignment alignment = runtime::FormatAlignment::None;
  std::uint64_t width = 0, precision = 0;
};
struct FormatPart {
  std::string text;
  bool argument = false;
  FormatSpec spec = {};
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
      parts.push_back({std::move(literal), false, {}});
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
    } else if (ch == '{') {
      const auto end = text.find('}', i + 1);
      if (end == std::string_view::npos)
        return std::unexpected(FormatTemplateError{i, "unclosed format placeholder"});
      const auto field = text.substr(i + 1, end - i - 1);
      FormatSpec spec;
      if (!field.empty()) {
        if (field.front() != ':')
          return std::unexpected(
              FormatTemplateError{i, "format placeholders are {} or {:control}"});
        std::size_t position = 1;
        const auto number = [&](std::uint64_t &value) {
          const auto begin = position;
          while (position < field.size() && field[position] >= '0' && field[position] <= '9') {
            const auto digit = unsigned(field[position++] - '0');
            if (value > (std::uint64_t(INT64_MAX) - digit) / 10)
              return false;
            value = value * 10 + digit;
          }
          return position != begin;
        };
        if (position < field.size() && (field[position] == '<' || field[position] == '>')) {
          spec.alignment = field[position++] == '<' ? runtime::FormatAlignment::Left
                                                    : runtime::FormatAlignment::Right;
          if (!number(spec.width))
            return std::unexpected(FormatTemplateError{
                i, "format alignment requires a nonnegative width in the String size domain"});
        }
        if (position < field.size() && field[position] == '.') {
          ++position;
          spec.presentation = runtime::FormatPresentation::Fixed;
          if (!number(spec.precision))
            return std::unexpected(
                FormatTemplateError{i, "fixed formatting requires a nonnegative decimal precision "
                                       "in the String size domain"});
        } else if (position < field.size() && field[position] == 'x') {
          ++position;
          spec.presentation = runtime::FormatPresentation::HexLower;
        }
        if (position != field.size() || (spec.alignment == runtime::FormatAlignment::None &&
                                         spec.presentation == runtime::FormatPresentation::Default))
          return std::unexpected(
              FormatTemplateError{i, "format controls accept <width, >width, .precision and x"});
      }
      if (used == arguments)
        return std::unexpected(FormatTemplateError{i, "format has more placeholders than arguments"});
      flush();
      parts.push_back({{}, true, spec});
      ++used;
      i = end + 1;
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
