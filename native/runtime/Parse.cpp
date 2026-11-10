#include "Parse.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <string_view>

namespace {
using namespace kalandra::runtime;
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
bool digit(char ch) noexcept { return ch >= '0' && ch <= '9'; }
void invalid(ParseScalarResult &out, std::size_t offset) noexcept {
  out = {};
  out.status = ParseStatus::Invalid;
  out.offset = offset;
}
void range(ParseScalarResult &out) noexcept {
  out = {};
  out.status = ParseStatus::OutOfRange;
}
// Every binary64 rounding midpoint is an integer times 2^-n, n <= 1075,
// with at most 54 significand bits. Its terminating decimal has fewer than
// 800 significant digits (54*log10(2) + 1075*log10(5) < 800).
// Retaining 800 digits plus a sticky nonzero tail therefore preserves every
// comparison with every f32/f64 rounding boundary. Source length is unlimited.
constexpr std::size_t decimalDigits = 800;
struct UInt {
  // 801 digits plus the largest binary64 quantization shift, and the decimal
  // denominator 10^(800+324), fit in 4096 bits. No heap allocation or FP state.
  std::array<std::uint32_t, 128> limbs{};
  unsigned size = 0;
  explicit UInt(std::uint32_t value = 0) noexcept {
    if (value) {
      limbs[0] = value;
      size = 1;
    }
  }
  void multiply10(unsigned add = 0) noexcept {
    std::uint64_t carry = add;
    for (unsigned i = 0; i < size; ++i) {
      carry += std::uint64_t(limbs[i]) * 10;
      limbs[i] = std::uint32_t(carry);
      carry >>= 32;
    }
    if (carry)
      limbs[size++] = std::uint32_t(carry);
  }
  unsigned bits() const noexcept {
    return size ? (size - 1) * 32 + 32 - std::countl_zero(limbs[size - 1]) : 0;
  }
  UInt shifted(unsigned n) const noexcept {
    UInt out;
    const auto words = n / 32, remainder = n % 32;
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < size; ++i) {
      const auto value = (std::uint64_t(limbs[i]) << remainder) | carry;
      out.limbs[i + words] = std::uint32_t(value);
      carry = value >> 32;
    }
    out.size = size ? size + words : 0;
    if (carry)
      out.limbs[out.size++] = std::uint32_t(carry);
    return out;
  }
  int compare(const UInt &other) const noexcept {
    if (size != other.size)
      return size < other.size ? -1 : 1;
    for (unsigned i = size; i-- > 0;)
      if (limbs[i] != other.limbs[i])
        return limbs[i] < other.limbs[i] ? -1 : 1;
    return 0;
  }
  void subtract(const UInt &other) noexcept {
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < size; ++i) {
      const auto part = (i < other.size ? std::uint64_t(other.limbs[i]) : 0) + borrow;
      const auto original = std::uint64_t(limbs[i]);
      limbs[i] = std::uint32_t(original - part);
      borrow = original < part;
    }
    while (size && !limbs[size - 1])
      --size;
  }
};
int combineExponent(bool negative, std::uint64_t magnitude, bool deltaNegative,
                    std::uint64_t delta) noexcept {
  if (negative == deltaNegative) {
    const auto sum =
        std::min<std::uint64_t>(10000, magnitude) + std::min<std::uint64_t>(10000, delta);
    return (negative ? -1 : 1) * int(std::min<std::uint64_t>(10000, sum));
  }
  const bool resultNegative = magnitude >= delta ? negative : deltaNegative;
  const auto distance = magnitude >= delta ? magnitude - delta : delta - magnitude;
  return (resultNegative ? -1 : 1) * int(std::min<std::uint64_t>(10000, distance));
}
void floating(Invocation &ctx, ParseScalarResult &out, std::string_view text, std::size_t begin,
              bool negative, bool wide) noexcept {
  const auto sign = negative ? (wide ? 1ULL << 63 : 1ULL << 31) : 0;
  if (begin < text.size() && (text[begin] == 'i' || text[begin] == 'n')) {
    const auto word = text[begin] == 'i' ? std::string_view("inf") : std::string_view("nan");
    for (std::size_t n = 0; n < word.size(); ++n)
      if (begin + n == text.size() || text[begin + n] != word[n]) {
        invalid(out, begin + n);
        return;
      }
    if (begin + word.size() != text.size()) {
      invalid(out, begin + word.size());
      return;
    }
    out.bits = sign | (wide ? 0x7ff0000000000000ULL : 0x7f800000ULL);
    if (word[0] == 'n')
      out.bits |= wide ? 0x8000000000000ULL : 0x400000ULL;
    return;
  }
  std::array<unsigned char, decimalDigits + 1> kept{};
  std::size_t count = 0, digits = 0, beforePoint = 0, first = 0;
  bool nonzero = false, sticky = false;
  std::size_t i = begin;
  const auto take = [&](char ch) {
    if (!nonzero && ch != '0') {
      nonzero = true;
      first = digits;
    }
    if (nonzero) {
      if (count < decimalDigits)
        kept[count++] = static_cast<unsigned char>(ch - '0');
      else
        sticky |= ch != '0';
    }
    ++digits;
  };
  while (i < text.size() && digit(text[i])) {
    if ((i & 1023) == 0 && !healthy(ctx))
      return;
    take(text[i++]);
    ++beforePoint;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    while (i < text.size() && digit(text[i])) {
      if ((i & 1023) == 0 && !healthy(ctx))
        return;
      take(text[i++]);
    }
  }
  if (!digits) {
    invalid(out, i);
    return;
  }
  bool exponentNegative = false;
  std::uint64_t exponent = 0;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
      exponentNegative = text[i++] == '-';
    const auto start = i;
    // Saturate above source length, not at a fixed exponent count: arbitrarily
    // many fraction/integer zeros can cancel a large explicit exponent.
    const auto limit = std::uint64_t(text.size()) + 10000;
    while (i < text.size() && digit(text[i])) {
      if ((i & 1023) == 0 && !healthy(ctx))
        return;
      const auto value = unsigned(text[i++] - '0');
      exponent = exponent > (limit - value) / 10 ? limit : exponent * 10 + value;
    }
    if (i == start) {
      invalid(out, i);
      return;
    }
  }
  if (i != text.size()) {
    invalid(out, i);
    return;
  }
  if (!healthy(ctx))
    return;
  if (!nonzero) {
    out.bits = sign;
    return;
  }
  const bool deltaNegative = beforePoint <= first;
  const auto delta = deltaNegative ? std::uint64_t(first - beforePoint + 1)
                                   : std::uint64_t(beforePoint - first - 1);
  const auto normalExponent = combineExponent(exponentNegative, exponent, deltaNegative, delta);
  if (normalExponent > (wide ? 308 : 38) || normalExponent < (wide ? -324 : -46)) {
    range(out);
    return;
  }
  if (sticky)
    kept[count++] = 1;
  UInt numerator;
  for (std::size_t n = 0; n < count; ++n)
    numerator.multiply10(kept[n]);
  UInt denominator(1);
  const auto power = normalExponent - int(count) + 1;
  for (int n = 0; n < (power < 0 ? -power : power); ++n) {
    if ((n & 31) == 0 && !healthy(ctx))
      return;
    (power < 0 ? denominator : numerator).multiply10();
  }
  int exponent2 = int(numerator.bits()) - int(denominator.bits());
  if ((exponent2 >= 0 ? numerator.compare(denominator.shifted(unsigned(exponent2)))
                      : numerator.shifted(unsigned(-exponent2)).compare(denominator)) < 0)
    --exponent2;
  const int precision = wide ? 53 : 24, minimum = wide ? -1022 : -126, maximum = wide ? 1023 : 127;
  auto quantizationExponent = std::max(exponent2, minimum);
  const auto scale = quantizationExponent - precision + 1;
  if (scale < 0)
    numerator = numerator.shifted(unsigned(-scale));
  else
    denominator = denominator.shifted(unsigned(scale));
  std::uint64_t significand = 0;
  for (int n = int(numerator.bits()) - int(denominator.bits()); n >= 0; --n) {
    if (!healthy(ctx))
      return;
    const auto divisor = denominator.shifted(unsigned(n));
    if (numerator.compare(divisor) >= 0) {
      numerator.subtract(divisor);
      significand |= 1ULL << n;
    }
  }
  const auto round = numerator.shifted(1).compare(denominator);
  if (round > 0 || (round == 0 && (significand & 1)))
    ++significand;
  if (!significand) {
    range(out);
    return;
  }
  if (significand == (1ULL << precision)) {
    significand >>= 1;
    ++quantizationExponent;
  }
  if (quantizationExponent > maximum) {
    range(out);
    return;
  }
  const auto hidden = 1ULL << (precision - 1);
  out.bits =
      sign | (significand < hidden
                  ? significand
                  : (std::uint64_t(quantizationExponent + (wide ? 1023 : 127)) << (precision - 1)) |
                        (significand - hidden));
}
void parse(Invocation &ctx, ParseScalarResult &out, ParseScalarKind kind,
           std::string_view text) noexcept {
  if (kind == ParseScalarKind::Bool) {
    const auto expected =
        !text.empty() && text[0] == 'f' ? std::string_view("false") : std::string_view("true");
    for (std::size_t i = 0; i < expected.size(); ++i)
      if (i == text.size() || text[i] != expected[i]) {
        invalid(out, i);
        return;
      }
    if (text.size() != expected.size()) {
      invalid(out, expected.size());
      return;
    }
    out.bits = expected[0] == 't';
    return;
  }
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-'))
    negative = text[i++] == '-';
  if (kind == ParseScalarKind::F32 || kind == ParseScalarKind::F64) {
    floating(ctx, out, text, i, negative, kind == ParseScalarKind::F64);
    return;
  }
  const bool signedType = kind <= ParseScalarKind::I64;
  if (negative && !signedType) {
    invalid(out, 0);
    return;
  }
  const auto ordinal =
      unsigned(kind) - unsigned(signedType ? ParseScalarKind::I8 : ParseScalarKind::U8);
  const auto width = 8u << ordinal;
  const auto maximum = signedType ? ((1ULL << (width - 1)) - (negative ? 0 : 1))
                                  : (width == 64 ? UINT64_MAX : (1ULL << width) - 1);
  if (i == text.size()) {
    invalid(out, i);
    return;
  }
  std::uint64_t value = 0;
  bool overflow = false;
  for (; i < text.size(); ++i) {
    if ((i & 1023) == 0 && !healthy(ctx))
      return;
    if (!digit(text[i])) {
      invalid(out, i);
      return;
    }
    const auto n = unsigned(text[i] - '0');
    if (value > maximum / 10 || (value == maximum / 10 && n > maximum % 10))
      overflow = true;
    if (!overflow)
      value = value * 10 + n;
  }
  if (overflow) {
    range(out);
    return;
  }
  out.bits = negative ? 0ULL - value : value;
  if (width < 64)
    out.bits &= (1ULL << width) - 1;
}
} // namespace
extern "C" void kalandra_parse_scalar_v1(Invocation *ctx, ParseScalarResult *out,
                                         std::uint32_t rawKind,
                                         const StringDescriptor *input) noexcept {
  *out = {};
  if (!healthy(*ctx))
    return;
  if (rawKind < unsigned(ParseScalarKind::I8) || rawKind > unsigned(ParseScalarKind::Bool)) {
    ctx->fault(3);
    return;
  }
  if (validateString(*ctx, *input))
    parse(*ctx, *out, static_cast<ParseScalarKind>(rawKind),
          std::string_view(input->data ? input->data : "", std::size_t(input->byteLength)));
  if (!healthy(*ctx))
    *out = {};
}
