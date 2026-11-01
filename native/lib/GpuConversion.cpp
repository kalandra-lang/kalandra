#include "kalandra/GpuConversion.h"
#include "kalandra/HardwareConversion.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include <algorithm>
namespace kalandra {
namespace {
using namespace mlir;
class Converter {
  OpBuilder &b;
  Location loc;
  Value c(std::uint64_t value) {
    return arith::ConstantOp::create(b, loc, b.getI64IntegerAttr(value));
  }
  Value power(unsigned bit) { return c(UINT64_C(1) << bit); }
  Value add(Value a, Value d) { return arith::AddIOp::create(b, loc, a, d); }
  Value sub(Value a, Value d) { return arith::SubIOp::create(b, loc, a, d); }
  Value band(Value a, Value d) { return arith::AndIOp::create(b, loc, a, d); }
  Value bor(Value a, Value d) { return arith::OrIOp::create(b, loc, a, d); }
  Value choose(Value p, Value a, Value d) { return arith::SelectOp::create(b, loc, p, a, d); }
  Value cmp(arith::CmpIPredicate p, Value a, Value d) {
    return arith::CmpIOp::create(b, loc, p, a, d);
  }
  Value eq(Value a, Value d) { return cmp(arith::CmpIPredicate::eq, a, d); }
  Value lt(Value a, Value d) { return cmp(arith::CmpIPredicate::slt, a, d); }
  Value yes() { return eq(c(0), c(0)); }
  Value no() { return eq(c(0), c(1)); }
  Value ext(Value value) { return arith::ExtUIOp::create(b, loc, b.getI64Type(), value); }
  Value left(Value v, Value n) {
    auto shifted = arith::ShLIOp::create(b, loc, v, band(n, c(63)));
    return choose(cmp(arith::CmpIPredicate::ult, n, c(64)), shifted, c(0));
  }
  Value right(Value v, Value n) {
    auto shifted = arith::ShRUIOp::create(b, loc, v, band(n, c(63)));
    return choose(cmp(arith::CmpIPredicate::ult, n, c(64)), shifted, c(0));
  }
  static std::uint64_t mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
  }

public:
  Converter(OpBuilder &builder, Location location) : b(builder), loc(location) {}
  std::pair<Value, Value> run(Value input, NumericType source, NumericType destination) {
    const auto src = describeNumeric(source), dst = describeNumeric(destination);
    input = band(input, c(mask(src->bits)));
    if (source == destination)
      return {input, yes()};
    Value magnitude, negative, exponent = c(0), nan = no(), infinity = nan;
    if (isInteger(source)) {
      negative = isUnsigned(source)
                     ? no()
                     : cmp(arith::CmpIPredicate::ne, band(input, power(src->bits - 1)), c(0));
      magnitude = band(choose(negative, sub(c(0), input), input), c(mask(src->bits)));
    } else {
      const unsigned fraction = src->bits == 32 ? 23 : 52, bias = src->bits == 32 ? 127 : 1023;
      const auto fieldMask = src->bits == 32 ? 255 : 2047;
      negative = cmp(arith::CmpIPredicate::ne, band(input, power(src->bits - 1)), c(0));
      auto field = band(right(input, c(fraction)), c(fieldMask));
      auto payload = band(input, c(mask(fraction)));
      auto special = eq(field, c(fieldMask));
      nan = band(special, cmp(arith::CmpIPredicate::ne, payload, c(0)));
      infinity = band(special, eq(payload, c(0)));
      magnitude = choose(eq(field, c(0)), payload, bor(payload, power(fraction)));
      exponent = sub(choose(eq(field, c(0)), c(1), field), c(bias + fraction));
    }
    auto finite = eq(bor(nan, infinity), no());
    if (isInteger(destination)) {
      auto positive = cmp(arith::CmpIPredicate::sge, exponent, c(0));
      // A 64-bit carrier must detect discarded high bits before shifting.
      auto overflow = band(
          positive, bor(cmp(arith::CmpIPredicate::uge, exponent, c(64)),
                        cmp(arith::CmpIPredicate::ugt, magnitude, right(c(UINT64_MAX), exponent))));
      auto value =
          choose(positive, left(magnitude, exponent), right(magnitude, sub(c(0), exponent)));
      auto bound = isUnsigned(destination)
                       ? c(mask(dst->bits))
                       : choose(negative, power(dst->bits - 1), c(mask(dst->bits - 1)));
      auto valid =
          band(finite, band(eq(overflow, no()), cmp(arith::CmpIPredicate::ule, value, bound)));
      if (isUnsigned(destination))
        valid = band(valid, bor(eq(negative, no()), eq(value, c(0))));
      value = choose(negative, sub(c(0), value), value);
      return {band(value, c(mask(dst->bits))), valid};
    }
    const unsigned fraction = dst->bits == 32 ? 23 : 52, bias = dst->bits == 32 ? 127 : 1023;
    const auto fieldMask = dst->bits == 32 ? 255 : 2047;
    auto lead = c(0), probe = magnitude;
    for (unsigned step = 32; step; step >>= 1) {
      auto move = cmp(arith::CmpIPredicate::uge, probe, power(step));
      probe = choose(move, right(probe, c(step)), probe);
      lead = choose(move, add(lead, c(step)), lead);
    }
    auto top = add(lead, exponent), quantum = sub(top, c(fraction));
    quantum = choose(lt(quantum, c(1 - int(bias) - int(fraction))),
                     c(1 - int(bias) - int(fraction)), quantum);
    auto distance = sub(quantum, exponent);
    auto quotient = right(magnitude, distance);
    auto remainder = band(magnitude, sub(left(c(1), distance), c(1)));
    auto half = left(c(1), sub(distance, c(1)));
    auto increment = bor(cmp(arith::CmpIPredicate::ugt, remainder, half),
                         band(eq(remainder, half), eq(band(quotient, c(1)), c(1))));
    auto rounded = add(quotient, ext(increment));
    rounded = choose(cmp(arith::CmpIPredicate::ugt, distance, c(64)), c(0), rounded);
    rounded = choose(cmp(arith::CmpIPredicate::sgt, distance, c(0)), rounded,
                     left(magnitude, sub(c(0), distance)));
    auto carry = cmp(arith::CmpIPredicate::uge, rounded, power(fraction + 1));
    rounded = choose(carry, right(rounded, c(1)), rounded);
    auto field = choose(cmp(arith::CmpIPredicate::uge, rounded, power(fraction)),
                        add(add(quantum, c(fraction + bias)), ext(carry)), c(0));
    auto overflow = cmp(arith::CmpIPredicate::sge, field, c(fieldMask));
    auto sign = choose(negative, power(dst->bits - 1), c(0));
    auto inf = c(std::uint64_t(fieldMask) << fraction);
    auto result = bor(sign, bor(left(field, c(fraction)), band(rounded, c(mask(fraction)))));
    result = choose(bor(infinity, overflow), bor(sign, inf), result);
    result = choose(nan, bor(inf, power(fraction - 1)), result);
    auto valid = bor(eq(finite, no()), eq(overflow, no()));
    return {band(result, c(mask(dst->bits))), valid};
  }
};
} // namespace
std::pair<mlir::Value, mlir::Value>
gpuNumericConversion(mlir::OpBuilder &builder, mlir::Location loc, mlir::Value input,
                     NumericType source, NumericType destination, ArithmeticMode arithmetic,
                     bool nativeFloat64) {
  auto result = Converter(builder, loc).run(input, source, destination);
  if (arithmetic == ArithmeticMode::Software || source == destination ||
      (!nativeFloat64 && (source == NumericType::Float64 || destination == NumericType::Float64)) ||
      (isInteger(source) && isInteger(destination)))
    return result;
  const auto src = describeNumeric(source), dst = describeNumeric(destination);
  mlir::Value decoded = input;
  if (src->bits < 64)
    decoded =
        mlir::arith::TruncIOp::create(builder, loc, builder.getIntegerType(src->bits), decoded);
  if (!isInteger(source))
    decoded = mlir::arith::BitcastOp::create(
        builder, loc, src->bits == 64 ? builder.getF64Type() : builder.getF32Type(), decoded);
  if (source == NumericType::Float64 || destination == NumericType::Float64) {
    // Keep the checked branch ABI in integer carriers. A software fallback can
    // then replace native casts without leaving floating-point SCF phi values
    // that would require Float64 capability just to transport their bits.
    using namespace mlir;
    if (isInteger(source) && src->bits < 32) {
      auto mask = arith::ConstantIntOp::create(builder, loc, (UINT64_C(1) << src->bits) - 1, 64);
      Value word = arith::AndIOp::create(builder, loc, input, mask);
      if (!isUnsigned(source)) {
        auto shift = arith::ConstantIntOp::create(builder, loc, 64 - src->bits, 64);
        word = arith::ShRSIOp::create(builder, loc,
                                      arith::ShLIOp::create(builder, loc, word, shift), shift);
      }
      decoded = arith::TruncIOp::create(builder, loc, builder.getI32Type(), word);
    }
    auto choice =
        scf::IfOp::create(builder, loc, TypeRange{builder.getI64Type()}, result.second, true, true);
    {
      OpBuilder::InsertionGuard guard(builder);
      builder.setInsertionPointToStart(&choice.getThenRegion().front());
      Type target = isInteger(destination)
                        ? Type(builder.getIntegerType(std::max(32u, unsigned(dst->bits))))
                    : dst->bits == 64 ? Type(builder.getF64Type())
                                      : Type(builder.getF32Type());
      Value converted;
      if (isInteger(source))
        converted = isUnsigned(source)
                        ? Value(arith::UIToFPOp::create(builder, loc, target, decoded))
                        : Value(arith::SIToFPOp::create(builder, loc, target, decoded));
      else if (isInteger(destination))
        converted = isUnsigned(destination)
                        ? Value(arith::FPToUIOp::create(builder, loc, target, decoded))
                        : Value(arith::FPToSIOp::create(builder, loc, target, decoded));
      else if (src->bits < dst->bits)
        converted = arith::ExtFOp::create(builder, loc, target, decoded);
      else
        converted = arith::TruncFOp::create(builder, loc, target, decoded);
      if (!isInteger(destination))
        converted =
            arith::BitcastOp::create(builder, loc, builder.getIntegerType(dst->bits), converted);
      if (dst->bits < 64) {
        converted = arith::ExtUIOp::create(builder, loc, builder.getI64Type(), converted);
        auto mask = arith::ConstantIntOp::create(builder, loc, (UINT64_C(1) << dst->bits) - 1, 64);
        converted = arith::AndIOp::create(builder, loc, converted, mask);
      }
      scf::YieldOp::create(builder, loc, ValueRange{converted});
      builder.setInsertionPointToStart(&choice.getElseRegion().front());
      auto zero = arith::ConstantIntOp::create(builder, loc, 0, 64);
      scf::YieldOp::create(builder, loc, ValueRange{zero});
    }
    result.first = choice.getResult(0);
    return result;
  }
  auto converted = hardwareConversion(builder, loc, decoded, source, destination, result.second);
  if (!isInteger(destination))
    converted = mlir::arith::BitcastOp::create(builder, loc, builder.getI32Type(), converted);
  if (dst->bits < 64) {
    converted = mlir::arith::ExtUIOp::create(builder, loc, builder.getI64Type(), converted);
    // SPIR-V can widen narrow scalar integers to 32 bits. Preserve the
    // canonical packed carrier even when the widened conversion sign-extends.
    auto mask =
        mlir::arith::ConstantIntOp::create(builder, loc, (std::uint64_t(1) << dst->bits) - 1, 64);
    converted = mlir::arith::AndIOp::create(builder, loc, converted, mask);
  }
  result.first = converted;
  return result;
}
} // namespace kalandra
