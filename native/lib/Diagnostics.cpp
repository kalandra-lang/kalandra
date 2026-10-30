#include "kalandra/Diagnostics.h"
#include "kalandra/Utf8.h"
#include <algorithm>
#include <map>

namespace kalandra {
namespace {
std::size_t unit(std::string_view text, std::size_t at) {
  const auto first = static_cast<unsigned char>(text[at]);
  if (first < 0x80)
    return 1;
  const std::size_t length = first >= 0xc2 && first <= 0xdf   ? 2
                             : first >= 0xe0 && first <= 0xef ? 3
                             : first >= 0xf0 && first <= 0xf4 ? 4
                                                              : 0;
  return length && length <= text.size() - at && validUtf8(text.substr(at, length)) ? length : 0;
}
std::string quote(std::string_view text) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (std::size_t i = 0; i < text.size();) {
    const auto value = static_cast<unsigned char>(text[i]);
    if (value >= 0x80) {
      auto length = unit(text, i);
      if (length) {
        result.append(text.substr(i, length));
        i += length;
      } else {
        result += "\ufffd";
        ++i;
      }
    } else {
      if (value == '"' || value == '\\') {
        result += '\\';
        result += char(value);
      } else if (value < 0x20) {
        result += "\\u00";
        result += hex[value >> 4];
        result += hex[value & 15];
      } else
        result += char(value);
      ++i;
    }
  }
  return result + '"';
}
struct Position {
  std::size_t line = 0, character = 0;
  std::string json() const {
    return "{\"line\":" + std::to_string(line) + ",\"character\":" + std::to_string(character) +
           "}";
  }
};
struct Location {
  const SourceFile *source = nullptr;
  std::size_t begin = 0, end = 0;
};
} // namespace
std::string renderDiagnosticsJson(const SourceFile *source, std::span<const Diagnostic> diagnostics,
                                  std::optional<CheckSummary> summary, std::string_view file) {
  std::vector<Location> locations;
  std::map<const SourceFile *, std::map<std::size_t, Position>> positions;
  for (const auto &diagnostic : diagnostics) {
    Location location;
    if (source) {
      auto [original, begin] = source->origin(diagnostic.span.begin);
      const auto length = diagnostic.span.end >= diagnostic.span.begin
                              ? diagnostic.span.end - diagnostic.span.begin
                              : 0;
      location = {original, begin, begin + std::min(length, original->text().size() - begin)};
      positions[original].try_emplace(location.begin);
      positions[original].try_emplace(location.end);
    }
    locations.push_back(location);
  }
  // One scan per original file, even for many diagnostics on a long line.
  for (auto &[original, offsets] : positions) {
    const auto &text = original->text();
    std::size_t at = 0;
    Position position;
    for (auto &[offset, output] : offsets) {
      while (at < offset) {
        const auto bytes = std::max<std::size_t>(1, unit(text, at));
        if (bytes > offset - at)
          break; // A byte inside a valid scalar maps to that scalar's start.
        if (text[at] == '\n' ||
            (text[at] == '\r' && (at + 1 == text.size() || text[at + 1] != '\n'))) {
          ++position.line;
          position.character = 0;
        } else if (!(text[at] == '\r' && at + 1 < text.size() && text[at + 1] == '\n'))
          position.character += bytes == 4 ? 2 : 1;
        at += bytes;
      }
      output = position;
    }
  }
  const bool ok = summary.has_value() && diagnostics.empty();
  std::string result = "{\"formatVersion\":1,\"positionEncoding\":\"utf-16\",\"ok\":";
  result += ok ? "true" : "false";
  result += ",\"diagnostics\":[";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i)
      result += ',';
    const auto &diagnostic = diagnostics[i];
    const auto &location = locations[i];
    result += "{\"severity\":\"error\",\"code\":" + quote(diagnostic.code) +
              ",\"message\":" + quote(diagnostic.message) + ",\"file\":";
    if (!location.source) {
      result += file.empty() ? "null" : quote(file);
      result += ",\"byteRange\":null,\"range\":null}";
    } else {
      const auto &offsets = positions.at(location.source);
      result += quote(location.source->path()) +
                ",\"byteRange\":{\"start\":" + std::to_string(location.begin) +
                ",\"end\":" + std::to_string(location.end) +
                "},\"range\":{\"start\":" + offsets.at(location.begin).json() +
                ",\"end\":" + offsets.at(location.end).json() + "}}";
    }
  }
  result += "],\"summary\":";
  if (ok)
    result += "{\"functions\":" + std::to_string(summary->functions) +
              ",\"values\":" + std::to_string(summary->values) + "}";
  else
    result += "null";
  return result + "}\n";
}
} // namespace kalandra
