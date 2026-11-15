#include "Format.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <string_view>

namespace {
using namespace kalandra::runtime;

bool copy(Invocation &context, char *output, const char *input, std::uint64_t bytes) noexcept {
  for (std::uint64_t offset = 0; offset < bytes;) {
    if (context.poll() != 0)
      return false;
    const auto count = std::min<std::uint64_t>(1024, bytes - offset);
    std::memcpy(output + offset, input + offset, count);
    offset += count;
  }
  return true;
}
bool fill(Invocation &context, char *output, char value, std::uint64_t bytes) noexcept {
  for (std::uint64_t offset = 0; offset < bytes;) {
    if (context.poll() != 0)
      return false;
    const auto count = std::min<std::uint64_t>(1024, bytes - offset);
    std::memset(output + offset, value, count);
    offset += count;
  }
  return true;
}
void publish(Invocation &context, StringDescriptor &output, std::string_view text,
             std::uint64_t zeroes = 0) noexcept {
  if (!zeroes) {
    kalandra_string_literal_v1(&context, &output, text.data(), text.size());
    return;
  }
  const auto size = combinedStringBytes(text.size(), zeroes);
  if (!size) {
    context.fault(2);
    return;
  }
  auto *data = static_cast<char *>(context.allocateRecord(*size));
  if (data && copy(context, data, text.data(), text.size()) &&
      fill(context, data + text.size(), '0', zeroes) && context.poll() == 0)
    output = {data, data, static_cast<std::int64_t>(*size)};
}
} // namespace

extern "C" void kalandra_format_number_v1(Invocation *context, StringDescriptor *output,
                                          std::uint32_t rawKind, std::uint64_t bits,
                                          std::uint32_t presentation,
                                          std::uint64_t precision) noexcept {
  *output = {};
  if (context->poll() != 0 || !context->usable())
    return;
  if (precision > std::uint64_t(INT64_MAX)) {
    context->fault(3);
    return;
  }
  const auto kind = static_cast<FormatScalarKind>(rawKind);
  if (presentation == unsigned(FormatPresentation::HexLower)) {
    if (rawKind < unsigned(FormatScalarKind::I8) || rawKind > unsigned(FormatScalarKind::U64) ||
        precision) {
      context->fault(3);
      return;
    }
    constexpr unsigned widths[] = {8, 16, 32, 64, 8, 16, 32, 64};
    const auto width = widths[rawKind - unsigned(FormatScalarKind::I8)];
    if (width < 64 && (bits >> width)) {
      context->fault(3);
      return;
    }
    char buffer[128];
    const auto convert = [&](auto value) {
      const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
      if (result.ec != std::errc{})
        context->fault(2);
      else
        publish(*context, *output, {buffer, std::size_t(result.ptr - buffer)});
    };
    switch (kind) {
    case FormatScalarKind::I8:
      convert(std::bit_cast<std::int8_t>(std::uint8_t(bits)));
      break;
    case FormatScalarKind::I16:
      convert(std::bit_cast<std::int16_t>(std::uint16_t(bits)));
      break;
    case FormatScalarKind::I32:
      convert(std::bit_cast<std::int32_t>(std::uint32_t(bits)));
      break;
    case FormatScalarKind::I64:
      convert(std::bit_cast<std::int64_t>(bits));
      break;
    default:
      convert(bits);
      break;
    }
    return;
  }
  if (presentation != unsigned(FormatPresentation::Fixed) ||
      (kind != FormatScalarKind::F32 && kind != FormatScalarKind::F64) ||
      (kind == FormatScalarKind::F32 && (bits >> 32))) {
    context->fault(3);
    return;
  }
  const bool nonfinite = kind == FormatScalarKind::F32
                             ? (bits & 0x7f800000) == 0x7f800000
                             : (bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL;
  if (nonfinite) {
    kalandra_format_scalar_v1(context, output, rawKind, bits);
    return;
  }
  // A finite binary64 has an exact decimal fraction in at most 1074 places.
  // Bound the uninterruptible library conversion; append further zeroes in
  // polled chunks instead of passing an arbitrarily large precision to it.
  constexpr std::uint64_t exactPlaces = 1074;
  const auto places = std::min(precision, exactPlaces);
  char buffer[1392]; // sign + 309 integer digits + dot + 1074 fractional digits
  const auto result =
      kind == FormatScalarKind::F32
          ? std::to_chars(buffer, buffer + sizeof(buffer),
                          std::bit_cast<float>(std::uint32_t(bits)), std::chars_format::fixed,
                          int(places))
          : std::to_chars(buffer, buffer + sizeof(buffer), std::bit_cast<double>(bits),
                          std::chars_format::fixed, int(places));
  if (result.ec != std::errc{}) {
    context->fault(2);
    return;
  }
  publish(*context, *output, {buffer, std::size_t(result.ptr - buffer)}, precision - places);
}

extern "C" void kalandra_format_align_v1(Invocation *context, StringDescriptor *output,
                                         const StringDescriptor *source, std::uint64_t width,
                                         std::uint32_t alignment) noexcept {
  if (context->poll() != 0 || !context->usable()) {
    *output = {};
    return;
  }
  if (!source || reinterpret_cast<std::uintptr_t>(source) % alignof(StringDescriptor) ||
      width > std::uint64_t(INT64_MAX) ||
      (alignment != unsigned(FormatAlignment::Left) &&
       alignment != unsigned(FormatAlignment::Right))) {
    *output = {};
    context->fault(3);
    return;
  }
  const auto input = *source; // The descriptor slots may alias.
  *output = {};
  if (!validateString(*context, input))
    return;
  std::uint64_t characters = 0;
  for (std::uint64_t i = 0; i < std::uint64_t(input.byteLength) && characters < width; ++i) {
    if ((i & 1023) == 0 && context->poll() != 0)
      return;
    if ((static_cast<unsigned char>(input.data[i]) & 0xc0) != 0x80)
      ++characters;
  }
  if (characters >= width) {
    if (context->poll() == 0)
      *output = input;
    return;
  }
  const auto padding = width - characters;
  const auto size = combinedStringBytes(input.byteLength, padding);
  if (!size) {
    context->fault(2);
    return;
  }
  auto *data = static_cast<char *>(context->allocateRecord(*size));
  if (!data)
    return;
  const bool right = alignment == unsigned(FormatAlignment::Right);
  if (fill(*context, data + (right ? 0 : input.byteLength), ' ', padding) &&
      copy(*context, data + (right ? padding : 0), input.data, input.byteLength) &&
      context->poll() == 0)
    *output = {data, data, static_cast<std::int64_t>(*size)};
}
