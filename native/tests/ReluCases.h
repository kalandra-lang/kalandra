#pragma once
#include "../include/kalandra/NumericType.h"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <stdexcept>
#include <vector>
namespace relu_tests {
inline void check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok)
    throw std::runtime_error(std::string(at.file_name()) + ":" + std::to_string(at.line()) +
                             ": ReLU conformance failure");
}
inline std::uint64_t read(const std::byte *at, unsigned width) {
  std::uint64_t value = 0;
  std::memcpy(&value, at, width);
  return value;
}
inline std::uint64_t expected(kalandra::NumericType type, std::uint64_t bits) {
  using namespace kalandra;
  if (isUnsigned(type))
    return bits;
  switch (type) {
  case NumericType::Float32: {
    float x = std::bit_cast<float>(std::uint32_t(bits));
    return std::isnan(x) || x > 0 ? bits : 0;
  }
  case NumericType::Float64: {
    double x = std::bit_cast<double>(bits);
    return std::isnan(x) || x > 0 ? bits : 0;
  }
  case NumericType::Int8:
    return std::bit_cast<std::int8_t>(std::uint8_t(bits)) < 0 ? 0 : bits;
  case NumericType::Int16:
    return std::bit_cast<std::int16_t>(std::uint16_t(bits)) < 0 ? 0 : bits;
  case NumericType::Int32:
    return std::bit_cast<std::int32_t>(std::uint32_t(bits)) < 0 ? 0 : bits;
  case NumericType::Int64:
    return std::bit_cast<std::int64_t>(bits) < 0 ? 0 : bits;
  default:
    throw std::runtime_error("Unknown ReLU oracle type");
  }
}
inline std::vector<std::byte> corpus(kalandra::NumericType type, std::size_t n) {
  const auto width = kalandra::describeNumeric(type)->bytes();
  const std::uint64_t f32[] = {0,          0x80000000, 0x3f800000, 0xbf800000, 0x7f800000,
                               0xff800000, 0x7fc12345, 0xffc12345, 0x7f812345, 0xff812345,
                               1,          0x80000001, 0x007fffff, 0x807fffff, 0x00800000,
                               0x80800000, 0x7f7fffff, 0xff7fffff};
  const std::uint64_t f64[] = {0,
                               0x8000000000000000,
                               0x3ff0000000000000,
                               0xbff0000000000000,
                               0x7ff0000000000000,
                               0xfff0000000000000,
                               0x7ff8123456789abc,
                               0xfff8123456789abc,
                               0x7ff0123456789abc,
                               0xfff0123456789abc,
                               1,
                               0x8000000000000001,
                               0x000fffffffffffff,
                               0x800fffffffffffff,
                               0x0010000000000000,
                               0x8010000000000000,
                               0x7fefffffffffffff,
                               0xffefffffffffffff};
  std::vector<std::byte> result(n * width);
  std::uint64_t state = 0x6a09e667f3bcc909;
  for (std::size_t i = 0; i < n; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    auto bits = state;
    if (kalandra::isInteger(type) && i < 8) {
      const auto sign = UINT64_C(1) << (width * 8 - 1);
      const auto mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
      const std::uint64_t edges[] = {0, 1, sign - 1, sign, sign + 1, mask, mask - 1, 2};
      bits = edges[i];
    } else if (i < 18)
      bits = width == 8 ? f64[i] : f32[i];
    std::memcpy(result.data() + i * width, &bits, width);
  }
  return result;
}
} // namespace relu_tests
