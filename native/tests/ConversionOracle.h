#pragma once
template <class T>
using Raw = std::conditional_t<
    sizeof(T) == 1, std::uint8_t,
    std::conditional_t<sizeof(T) == 2, std::uint16_t,
                       std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
struct Expected {
  std::uint64_t bits;
  bool valid;
};
static Expected reference(std::uint64_t bits) {
  const auto src = describeNumeric(source), dst = describeNumeric(destination);
  if (source == destination)
    return {bits, true};
  llvm::APInt integer(src->bits, bits);
  if (isInteger(source) && isInteger(destination)) {
    __int128_t value = isUnsigned(source) ? __int128_t(integer.getZExtValue())
                                          : __int128_t(integer.getSExtValue());
    __int128_t minimum = isUnsigned(destination) ? 0 : -(__int128_t(1) << (dst->bits - 1));
    __int128_t maximum = isUnsigned(destination) ? ((__int128_t(1) << dst->bits) - 1)
                                                 : ((__int128_t(1) << (dst->bits - 1)) - 1);
    return {std::uint64_t(value) & (dst->bits == 64 ? UINT64_MAX : (UINT64_C(1) << dst->bits) - 1),
            value >= minimum && value <= maximum};
  }
  const auto &semantics =
      source == NumericType::Float32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  llvm::APFloat value(semantics);
  if (isInteger(source)) {
    llvm::APFloat result(destination == NumericType::Float32 ? llvm::APFloat::IEEEsingle()
                                                             : llvm::APFloat::IEEEdouble());
    result.convertFromAPInt(integer, !isUnsigned(source), llvm::APFloat::rmNearestTiesToEven);
    return {result.bitcastToAPInt().getZExtValue(), true};
  }
  value = llvm::APFloat(semantics, integer);
  if (isInteger(destination)) {
    llvm::APSInt result(dst->bits, isUnsigned(destination));
    bool exact = false;
    auto status = value.convertToInteger(result, llvm::APFloat::rmTowardZero, &exact);
    return {result.getZExtValue(), !(status & llvm::APFloat::opInvalidOp)};
  }
  const bool finite = value.isFinite();
  bool loses = false;
  auto status = value.convert(destination == NumericType::Float32 ? llvm::APFloat::IEEEsingle()
                                                                  : llvm::APFloat::IEEEdouble(),
                              llvm::APFloat::rmNearestTiesToEven, &loses);
  const auto out = value.isNaN() ? (dst->bits == 32 ? UINT64_C(0x7fc00000) : Nan)
                                 : value.bitcastToAPInt().getZExtValue();
  return {out, !(finite && (status & llvm::APFloat::opOverflow))};
}
static std::vector<std::uint64_t> conversionInputs() {
  std::vector<std::uint64_t> inputs;
  if (describeNumeric(source)->bits <= 16) {
    for (unsigned value = 0; value < (UINT64_C(1) << describeNumeric(source)->bits); ++value)
      inputs.push_back(value);
    return inputs;
  }
  const auto width = describeNumeric(source)->bits;
  const auto mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
  for (auto value : {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_MAX, UINT64_MAX - 1,
                     UINT64_C(0x8000000000000000), UINT64_C(0x7fffffffffffffff)})
    inputs.push_back(value & mask);
  if (isInteger(source)) {
    for (unsigned bit = 0; bit < width; ++bit)
      for (std::uint64_t delta : {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_C(3), UINT64_C(4),
                                  UINT64_C(1023), UINT64_C(1024), UINT64_C(1025), UINT64_MAX}) {
        auto value = ((UINT64_C(1) << bit) + delta) & mask;
        inputs.push_back(value);
        inputs.push_back((0 - value) & mask);
      }
  } else {
    const unsigned fraction = width == 32 ? 23 : 52, exponents = width == 32 ? 256 : 2048;
    const auto significand = (UINT64_C(1) << fraction) - 1, sign = UINT64_C(1) << (width - 1);
    for (unsigned exponent = 0; exponent < exponents; ++exponent)
      for (auto tail : {UINT64_C(0), UINT64_C(1), significand / 2, significand / 2 + 1,
                        significand - 1, significand}) {
        auto bits = (std::uint64_t(exponent) << fraction) | tail;
        inputs.push_back(bits);
        inputs.push_back(bits | sign);
      }
  }
  if (!isInteger(source)) {
    const unsigned fraction = width == 32 ? 23 : 52, bias = width == 32 ? 127 : 1023;
    for (unsigned power = 0; power <= 64; ++power) {
      auto bits = std::uint64_t(power + bias) << fraction;
      for (std::uint64_t delta : {UINT64_C(0), UINT64_C(1), UINT64_MAX}) {
        inputs.push_back(bits + delta);
        inputs.push_back((bits + delta) | (UINT64_C(1) << (width - 1)));
      }
    }
    if (width == 64) {
      for (std::uint32_t bits :
           {0u, 1u, 2u, 0x007fffffu, 0x00800000u, 0x3f800000u, 0x3f800001u, 0x7f7fffffu}) {
        llvm::APFloat value(llvm::APFloat::IEEEsingle(), llvm::APInt(32, bits));
        bool loses = false;
        value.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &loses);
        const auto expanded = value.bitcastToAPInt().getZExtValue();
        for (std::uint64_t delta : {UINT64_C(0), UINT64_C(1), UINT64_MAX, UINT64_C(1) << 28,
                                    (UINT64_C(1) << 28) - 1, (UINT64_C(1) << 28) + 1}) {
          inputs.push_back(expanded + delta);
          inputs.push_back((expanded + delta) | Sign);
        }
      }
    }
  }
  std::uint64_t seed = 0x98da341ef075bc62;
  for (unsigned i = 0; i < 8192; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    inputs.push_back(seed & mask);
  }
  return inputs;
}
