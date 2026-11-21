#include "DependencyManifest.h"
#include <algorithm>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <vector>

namespace kalandra::detail {
namespace {
struct Span {
  std::size_t begin;
  std::size_t end;
};

// toml++ columns count Unicode scalars, while string offsets count UTF-8 bytes.
class Locations {
  const std::string &text;
  std::vector<std::size_t> lines;

public:
  explicit Locations(const std::string &source)
      : text(source), lines{source.starts_with("\xEF\xBB\xBF") ? 3u : 0u} {
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        lines.push_back(i + 1);
  }
  std::size_t offset(toml::source_position pos) const {
    auto byte = lines.at(pos.line - 1);
    for (std::size_t column = 1; column < pos.column && byte < text.size(); ++column) {
      ++byte;
      while (byte < text.size() && (static_cast<unsigned char>(text[byte]) & 0xc0) == 0x80)
        ++byte;
    }
    return byte;
  }
  Span statement(const toml::node &value) const {
    const auto region = value.source();
    const auto begin = lines.at(region.begin.line - 1);
    const auto end = offset(region.end);
    const auto suffix = text.find_first_not_of(" \t\r", end);
    // Retain an attached comment as a standalone comment.
    if (suffix != std::string::npos && text[suffix] == '#')
      return {begin, suffix};
    return {begin, suffix == std::string::npos ? text.size() : suffix + 1};
  }
};

bool literalTable(const toml::table &table, const Locations &locations, const std::string &source) {
  return source.at(locations.offset(table.source().begin)) == '{';
}

// An inline dependencies table may interleave dotted fields from several aliases.
// Walk complete TOML values, so commas in strings or nested tables stay untouched.
std::string inlineEntries(const toml::table &dependencies, std::string_view alias,
                          const Locations &locations, const std::string &source, Span body) {
  struct Entry {
    std::size_t end;
    bool keep;
  };
  std::vector<Entry> entries;
  for (const auto &[key, request] : dependencies) {
    if (literalTable(*request.as_table(), locations, source))
      entries.push_back({locations.offset(request.source().end), key != alias});
    else
      for (const auto &[field, value] : *request.as_table())
        entries.push_back({locations.offset(value.source().end), key != alias});
  }
  std::ranges::sort(entries, {}, &Entry::end);
  std::string result;
  auto start = body.begin;
  for (const auto &[end, keep] : entries) {
    const auto comma = source.find_first_not_of(" \t", end);
    if (comma == std::string::npos || comma > body.end)
      throw std::runtime_error("cannot locate inline dependency separator");
    if (keep) {
      if (!result.empty())
        result += ',';
      result.append(source, start, comma - start);
    }
    start = comma + 1;
  }
  return result;
}
} // namespace

std::string withoutDependency(const std::string &source, std::string_view alias) {
  auto expected = toml::parse(source);
  auto *dependencies = expected["dependencies"].as_table();
  auto *request = dependencies ? (*dependencies)[alias].as_table() : nullptr;
  if (!request)
    throw std::runtime_error("unknown direct dependency '" + std::string(alias) + "'");
  Locations locations(source);
  auto result = source;
  if (literalTable(*dependencies, locations, source)) {
    const Span body{locations.offset(dependencies->source().begin) + 1,
                    locations.offset(dependencies->source().end) - 1};
    result.replace(body.begin, body.end - body.begin,
                   inlineEntries(*dependencies, alias, locations, source, body));
  } else {
    std::vector<Span> spans;
    if (literalTable(*request, locations, source)) {
      spans.push_back(locations.statement(*request));
    } else {
      if (source.at(locations.offset(request->source().begin)) == '[')
        spans.push_back(locations.statement(*request));
      for (const auto &[field, value] : *request)
        spans.push_back(locations.statement(value));
    }
    std::ranges::sort(spans, std::greater{}, &Span::begin);
    for (auto [begin, end] : spans)
      result.erase(begin, end - begin);
  }
  dependencies->erase(alias);
  if (dependencies->empty())
    expected.erase("dependencies");
  auto actual = toml::parse(result);
  if (auto *table = actual["dependencies"].as_table(); table && table->empty())
    actual.erase("dependencies");
  if (actual != expected)
    throw std::runtime_error("dependency edit changed unrelated TOML declarations");
  return result;
}
} // namespace kalandra::detail
