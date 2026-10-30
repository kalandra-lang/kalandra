#pragma once
#include "kalandra/Utf8.h"
#include <optional>
#include <string>
namespace kalandra {
// Input includes the source quotes; the lexer has already bounded the token.
inline std::optional<std::string> decodeStringLiteral(std::string_view source) {
  if (source.size() < 2 || source.front() != '"' || source.back() != '"')
    return {};
  std::string out;
  for (std::size_t i = 1; i < source.size() - 1; ++i) {
    const char c = source[i];
    if (c == '\n' || c == '\r' || c == '"')
      return {};
    if (c != '\\') {
      out += c;
      continue;
    }
    if (++i >= source.size() - 1)
      return {};
    switch (source[i]) {
    case '"':
      out += '"';
      break;
    case '\\':
      out += '\\';
      break;
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    case 't':
      out += '\t';
      break;
    case '0':
      out += '\0';
      break;
    case 'u': {
      if (++i >= source.size() - 1 || source[i] != '{')
        return {};
      std::uint32_t point = 0;
      unsigned digits = 0;
      while (++i < source.size() - 1 && source[i] != '}') {
        const char h = source[i];
        const int n = h >= '0' && h <= '9'   ? h - '0'
                      : h >= 'a' && h <= 'f' ? h - 'a' + 10
                      : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                             : -1;
        if (n < 0 || ++digits > 6)
          return {};
        point = (point << 4) | n;
      }
      if (i >= source.size() - 1 || !digits || point > 0x10ffff ||
          (point >= 0xd800 && point <= 0xdfff))
        return {};
      if (point < 0x80)
        out += static_cast<char>(point);
      else {
        const unsigned count = point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
        out += static_cast<char>((count == 2   ? 0xc0
                                  : count == 3 ? 0xe0
                                               : 0xf0) |
                                 (point >> (6 * (count - 1))));
        for (unsigned j = count - 1; j; --j)
          out += static_cast<char>(0x80 | ((point >> (6 * (j - 1))) & 0x3f));
      }
      break;
    }
    default:
      return {};
    }
  }
  if (!validUtf8(out))
    return {};
  return out;
}
} // namespace kalandra
