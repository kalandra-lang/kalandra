#include "Format.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <string_view>

static_assert(sizeof(kalandra::runtime::StringDescriptor) ==
              kalandra::runtime::formatDescriptorBytes);
namespace {
template <class T>
std::string_view convert(char (&buffer)[128], T value) noexcept {
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return result.ec == std::errc{} ? std::string_view(buffer, result.ptr - buffer)
                                : std::string_view{};
}
} // namespace
extern "C" void kalandra_format_scalar_v1(kalandra::runtime::Invocation *context,
                                           kalandra::runtime::StringDescriptor *output,
                                           std::uint32_t rawKind, std::uint64_t bits) noexcept {
  using kalandra::runtime::FormatScalarKind;
  *output = {};
  if (context->poll() != 0 || !context->usable())
    return;
  const auto kind = static_cast<FormatScalarKind>(rawKind);
  unsigned width = 64;
  switch (kind) {
  case FormatScalarKind::I8: case FormatScalarKind::U8: width = 8; break;
  case FormatScalarKind::I16: case FormatScalarKind::U16: width = 16; break;
  case FormatScalarKind::I32: case FormatScalarKind::U32: case FormatScalarKind::F32: width = 32; break;
  case FormatScalarKind::Bool: width = 1; break;
  case FormatScalarKind::I64: case FormatScalarKind::U64: case FormatScalarKind::F64: break;
  default: context->fault(3); return;
  }
  if (width < 64 && (bits >> width)) {
    context->fault(3);
    return;
  }
  char buffer[128];
  std::string_view text;
  switch (kind) {
  case FormatScalarKind::I8: text = convert(buffer, std::bit_cast<std::int8_t>(std::uint8_t(bits))); break;
  case FormatScalarKind::I16: text = convert(buffer, std::bit_cast<std::int16_t>(std::uint16_t(bits))); break;
  case FormatScalarKind::I32: text = convert(buffer, std::bit_cast<std::int32_t>(std::uint32_t(bits))); break;
  case FormatScalarKind::I64: text = convert(buffer, std::bit_cast<std::int64_t>(bits)); break;
  case FormatScalarKind::U8: case FormatScalarKind::U16: case FormatScalarKind::U32:
  case FormatScalarKind::U64: text = convert(buffer, bits); break;
  case FormatScalarKind::Bool: text = bits ? "true" : "false"; break;
  case FormatScalarKind::F32:
    if ((bits & 0x7f800000) == 0x7f800000)
      text = (bits & 0x7fffff) ? "nan" : ((bits >> 31) ? "-inf" : "inf");
    else
      text = convert(buffer, std::bit_cast<float>(std::uint32_t(bits)));
    break;
  case FormatScalarKind::F64:
    if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
      text = (bits & 0xfffffffffffffULL) ? "nan" : ((bits >> 63) ? "-inf" : "inf");
    else
      text = convert(buffer, std::bit_cast<double>(bits));
    break;
  }
  if (text.empty()) {
    context->fault(2);
    return;
  }
  kalandra_string_literal_v1(context, output, text.data(), text.size());
}

extern "C" void kalandra_format_join_v1(kalandra::runtime::Invocation *context,
                                        kalandra::runtime::StringDescriptor *output,
                                        const kalandra::runtime::StringDescriptor *pieces,
                                        std::uint64_t count) noexcept {
  using kalandra::runtime::StringDescriptor;
  if (context->poll() != 0 || !context->usable() || !count) {
    *output = {};
    return;
  }
  if (!pieces || count > std::uint64_t(INT64_MAX) / sizeof(StringDescriptor) ||
      reinterpret_cast<std::uintptr_t>(pieces) % alignof(StringDescriptor)) {
    *output = {};
    context->fault(3);
    return;
  }
  auto table = context->lease(pieces);
  if (!table || table->bytes().size() < count * sizeof(StringDescriptor)) {
    *output = {};
    context->fault(3);
    return;
  }
  std::optional<std::uint64_t> alias;
  StringDescriptor saved;
  for (std::uint64_t i = 0; i < count; ++i) {
    if ((i & 255) == 0 && context->poll() != 0) {
      *output = {};
      return;
    }
    if (output == pieces + i) {
      alias = i;
      saved = pieces[i];
      break;
    }
  }
  *output = {};
  const auto at = [&](std::uint64_t i) { return alias == i ? saved : pieces[i]; };
  std::uint64_t length = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto part = at(i);
    if (!kalandra::runtime::validateString(*context, part))
      return;
    const auto sum = kalandra::runtime::combinedStringBytes(length, part.byteLength);
    if (!sum) {
      context->fault(2);
      return;
    }
    length = *sum;
  }
  if (!length)
    return;
  auto *data = static_cast<char *>(context->allocateRecord(length));
  if (!data)
    return;
  std::uint64_t offset = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (context->poll() != 0)
      return;
    const auto part = at(i);
    for (std::uint64_t next = 0; next < std::uint64_t(part.byteLength);) {
      if (context->poll() != 0)
        return;
      const auto bytes = std::min<std::uint64_t>(1024, part.byteLength - next);
      std::memcpy(data + offset, part.data + next, bytes);
      offset += bytes;
      next += bytes;
    }
  }
  if (context->poll() == 0)
    *output = {data, data, static_cast<std::int64_t>(length)};
}
