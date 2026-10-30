#include "Edits.h"
#include "kalandra/Utf8.h"
#include <algorithm>
namespace kalandra::lsp {
namespace {
// Input is validated UTF-8; supplementary scalars use four bytes and two units.
std::size_t width(unsigned char c) { return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
std::optional<std::size_t> offset(std::string_view text, Position position) {
  std::size_t at = 0, line = 0;
  while (line < position.line && at < text.size()) {
    if (text[at] == '\r') {
      ++at;
      if (at < text.size() && text[at] == '\n')
        ++at;
      ++line;
    } else if (text[at++] == '\n')
      ++line;
  }
  if (line != position.line)
    return {};
  std::size_t units = 0;
  while (at < text.size() && text[at] != '\r' && text[at] != '\n' && units < position.character) {
    const auto bytes = width(static_cast<unsigned char>(text[at]));
    const auto count = bytes == 4 ? 2u : 1u;
    if (count > position.character - units)
      return {}; // Split surrogate pair.
    units += count;
    at += bytes;
  }
  return at; // LSP character positions beyond line length clamp to the line end.
}
std::size_t utf16Length(std::string_view text) {
  std::size_t units = 0;
  for (std::size_t at = 0; at < text.size();) {
    const auto bytes = width(static_cast<unsigned char>(text[at]));
    at += bytes;
    units += bytes == 4 ? 2 : 1;
  }
  return units;
}
bool after(Position a, Position b) {
  return a.line > b.line || (a.line == b.line && a.character > b.character);
}
} // namespace
std::optional<std::size_t> textOffset(std::string_view text, Position position) {
  if (!validUtf8(text))
    return {};
  return offset(text, position);
}
std::expected<std::string, std::string>
applyChanges(std::string_view original, std::span<const TextChange> changes, EditLimits limits) {
  if (original.size() > limits.maxBytes || original.size() > limits.maxWorkBytes ||
      changes.size() > limits.maxChanges)
    return std::unexpected("document edit limits exceeded");
  if (!validUtf8(original))
    return std::unexpected("document is not valid UTF-8");
  std::size_t work = original.size();
  std::string text(original);
  for (const auto &change : changes) {
    const auto charge = [&](std::size_t count) {
      if (count > limits.maxWorkBytes - work)
        return false;
      work += count;
      return true;
    };
    if (!charge(change.text.size()) || !charge(1) || (change.range && !charge(text.size())))
      return std::unexpected("document edit work limit exceeded");
    if (!validUtf8(change.text))
      return std::unexpected("replacement is not valid UTF-8");
    std::size_t begin = 0, end = text.size();
    if (change.range) {
      if (after(change.range->start, change.range->end))
        return std::unexpected("document edit range is reversed");
      auto start = offset(text, change.range->start), stop = offset(text, change.range->end);
      if (!start || !stop || *start > *stop)
        return std::unexpected("document edit range has an invalid line or splits a UTF-16 scalar");
      begin = *start;
      end = *stop;
      if (change.rangeLength &&
          *change.rangeLength != utf16Length(std::string_view(text).substr(begin, end - begin)))
        return std::unexpected("document edit rangeLength does not match its UTF-16 range");
    } else if (change.rangeLength)
      return std::unexpected("rangeLength requires a document edit range");
    const auto retained = text.size() - (end - begin);
    if (change.text.size() > limits.maxBytes - retained)
      return std::unexpected("document edit exceeds open-buffer byte budget");
    text.replace(begin, end - begin, change.text);
  }
  return text;
}
} // namespace kalandra::lsp
