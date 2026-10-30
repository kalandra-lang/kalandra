#pragma once
#include <cstdint>
#include <string_view>
namespace kalandra {
enum class Utf8Status { Valid, Invalid, Interrupted };
struct Utf8Range {
  std::uint64_t start, end;
  std::size_t offset = 0, bytes = 0;
  bool valid = false;
};
struct Utf8Scalar {
  std::uint64_t index;
  std::size_t offset = 0, bytes = 0;
};
// A strict scalar decoder. Poll between bounded groups before touching bytes.
template <class Poll>
Utf8Status validateUtf8(std::string_view text, Poll poll, std::uint64_t *scalars = nullptr,
                        Utf8Scalar *selected = nullptr, Utf8Range *range = nullptr) {
  std::size_t count = 0, selectedOffset = 0, selectedBytes = 0;
  std::size_t rangeStart = 0, rangeEnd = 0;
  bool foundStart = false, foundEnd = false;
  for (std::size_t i = 0; i < text.size();) {
    if ((count++ & 255) == 0 && !poll())
      return Utf8Status::Interrupted;
    if (range) {
      if (range->start == count - 1) {
        rangeStart = i;
        foundStart = true;
      }
      if (range->end == count - 1) {
        rangeEnd = i;
        foundEnd = true;
      }
    }
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80) {
      if (selected && selected->index == count - 1) {
        selectedOffset = i;
        selectedBytes = 1;
      }
      ++i;
      continue;
    }
    const unsigned length = first >= 0xc2 && first <= 0xdf   ? 2
                            : first >= 0xe0 && first <= 0xef ? 3
                            : first >= 0xf0 && first <= 0xf4 ? 4
                                                             : 0;
    if (!length || length > text.size() - i)
      return Utf8Status::Invalid;
    std::uint32_t point = first & (0x7f >> length);
    for (unsigned j = 1; j < length; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80)
        return Utf8Status::Invalid;
      point = (point << 6) | (next & 0x3f);
    }
    if (point < (length == 2   ? 0x80u
                 : length == 3 ? 0x800u
                               : 0x10000u) ||
        point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
      return Utf8Status::Invalid;
    if (selected && selected->index == count - 1) {
      selectedOffset = i;
      selectedBytes = length;
    }
    i += length;
  }
  if (!poll())
    return Utf8Status::Interrupted;
  if (scalars)
    *scalars = count;
  if (selected) {
    selected->offset = selectedOffset;
    selected->bytes = selectedBytes;
  }
  if (range) {
    if (range->start == count) {
      rangeStart = text.size();
      foundStart = true;
    }
    if (range->end == count) {
      rangeEnd = text.size();
      foundEnd = true;
    }
    range->valid = foundStart && foundEnd && range->start <= range->end;
    range->offset = range->valid ? rangeStart : 0;
    range->bytes = range->valid ? rangeEnd - rangeStart : 0;
  }
  return Utf8Status::Valid;
}
inline bool validUtf8(std::string_view text) {
  return validateUtf8(text, [] { return true; }) == Utf8Status::Valid;
}
} // namespace kalandra
