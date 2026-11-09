#include "Format.h"
#include <bit>
#include <charconv>
#include <string_view>

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
