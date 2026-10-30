#include "kalandra/NumericConversion.h"
#include "kalandra/HardwareConversion.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
namespace kalandra {
namespace {
using namespace mlir;
class Converter {
  OpBuilder &b;
  Location loc;
  IntegerType wide;
  Value c(std::int64_t n) {
    return arith::ConstantOp::create(b, loc, b.getIntegerAttr(wide, llvm::APInt(128, n, true)));
  }
  Value power(unsigned n) {
    return arith::ConstantOp::create(b, loc,
                                     b.getIntegerAttr(wide, llvm::APInt::getOneBitSet(128, n)));
  }
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
  Value ext(Value v) { return arith::ExtUIOp::create(b, loc, wide, v); }
  // Select arms are evaluated eagerly. Clamp every emitted shift, even when validity
  // will reject the source or a different arm will supply the final value.
  Value shift(Value n) { return choose(lt(n, c(0)), c(0), choose(lt(n, c(128)), n, c(127))); }
  Value left(Value v, Value n) { return arith::ShLIOp::create(b, loc, v, shift(n)); }
  Value right(Value v, Value n) { return arith::ShRUIOp::create(b, loc, v, shift(n)); }
  Value yes() { return eq(c(0), c(0)); }

public:
  Converter(OpBuilder &builder, Location location)
      : b(builder), loc(location), wide(b.getIntegerType(128)) {}
  std::pair<Value, Value> run(Value input, NumericType source, NumericType destination) {
    if (source == destination)
      return {input, yes()};
    const auto src = describeNumeric(source), dst = describeNumeric(destination);
    // Finite input magnitude is an unsigned significand times 2^exponent.
    // i128 holds every possible 64-bit integer and intermediate significand.
    Value magnitude, negative, exponent = c(0), nan = eq(c(0), c(1)), infinity = nan;
    if (isInteger(source)) {
      Value bits =
          isUnsigned(source) ? ext(input) : Value(arith::ExtSIOp::create(b, loc, wide, input));
      negative = isUnsigned(source) ? nan : lt(bits, c(0));
      magnitude = choose(negative, sub(c(0), bits), bits);
    } else {
      const unsigned fraction = src->bits == 32 ? 23 : 52, bias = src->bits == 32 ? 127 : 1023;
      const auto mask = src->bits == 32 ? 255 : 2047;
      auto bits = ext(arith::BitcastOp::create(b, loc, b.getIntegerType(src->bits), input));
      negative = cmp(arith::CmpIPredicate::ne, band(bits, power(src->bits - 1)), c(0));
      auto field = band(right(bits, c(fraction)), c(mask));
      auto payload = band(bits, sub(power(fraction), c(1)));
      auto special = eq(field, c(mask));
      nan = band(special, cmp(arith::CmpIPredicate::ne, payload, c(0)));
      infinity = band(special, eq(payload, c(0)));
      magnitude = choose(eq(field, c(0)), payload, bor(payload, power(fraction)));
      exponent = sub(choose(eq(field, c(0)), c(1), field), c(bias + fraction));
    }
    auto finite = eq(bor(nan, infinity), eq(c(0), c(1)));
    if (isInteger(destination)) {
      auto positiveShift = cmp(arith::CmpIPredicate::sge, exponent, c(0));
      auto overflow = band(positiveShift, cmp(arith::CmpIPredicate::sge, exponent, c(64)));
      auto value =
          choose(positiveShift, left(magnitude, exponent), right(magnitude, sub(c(0), exponent)));
      // Right shifts beyond the complete significand truncate to zero.
      value = choose(lt(exponent, c(-127)), c(0), value);
      auto bound = isUnsigned(destination)
                       ? sub(power(dst->bits), c(1))
                       : choose(negative, power(dst->bits - 1), sub(power(dst->bits - 1), c(1)));
      auto valid = band(
          finite, band(eq(overflow, eq(c(0), c(1))), cmp(arith::CmpIPredicate::ule, value, bound)));
      if (isUnsigned(destination))
        valid = band(valid, bor(eq(negative, eq(c(0), c(1))), eq(value, c(0))));
      value = choose(negative, sub(c(0), value), value);
      return {arith::TruncIOp::create(b, loc, b.getIntegerType(dst->bits), value), valid};
    }
    const unsigned fraction = dst->bits == 32 ? 23 : 52, bias = dst->bits == 32 ? 127 : 1023;
    const auto mask = dst->bits == 32 ? 255 : 2047;
    // Find the leading bit in seven bounded steps, then select the destination
    // normal/subnormal quantum. Round discarded bits once, with ties to even.
    auto lead = c(0), probe = magnitude;
    for (unsigned step = 64; step; step >>= 1) {
      auto move = cmp(arith::CmpIPredicate::uge, probe, power(step));
      probe = choose(move, right(probe, c(step)), probe);
      lead = choose(move, add(lead, c(step)), lead);
    }
    auto top = add(lead, exponent);
    auto quantum = sub(top, c(fraction));
    quantum = choose(lt(quantum, c(1 - int(bias) - int(fraction))),
                     c(1 - int(bias) - int(fraction)), quantum);
    auto distance = sub(quantum, exponent);
    auto quotient = right(magnitude, distance);
    auto remainder = band(magnitude, sub(left(c(1), distance), c(1)));
    auto half = left(c(1), sub(distance, c(1)));
    auto increment = bor(cmp(arith::CmpIPredicate::ugt, remainder, half),
                         band(eq(remainder, half), eq(band(quotient, c(1)), c(1))));
    auto rounded = add(quotient, ext(increment));
    rounded = choose(cmp(arith::CmpIPredicate::sge, distance, c(128)), c(0), rounded);
    rounded = choose(cmp(arith::CmpIPredicate::sgt, distance, c(0)), rounded,
                     left(magnitude, sub(c(0), distance)));
    auto carry = cmp(arith::CmpIPredicate::uge, rounded, power(fraction + 1));
    rounded = choose(carry, right(rounded, c(1)), rounded);
    auto field = choose(cmp(arith::CmpIPredicate::uge, rounded, power(fraction)),
                        add(add(quantum, c(fraction + bias)), ext(carry)), c(0));
    auto overflow = cmp(arith::CmpIPredicate::sge, field, c(mask));
    auto sign = choose(negative, power(dst->bits - 1), c(0));
    auto inf = left(c(mask), c(fraction));
    auto bits = bor(sign, bor(left(field, c(fraction)), band(rounded, sub(power(fraction), c(1)))));
    bits = choose(bor(infinity, overflow), bor(sign, inf), bits);
    bits = choose(nan, bor(inf, power(fraction - 1)), bits);
    auto valid = bor(eq(finite, eq(c(0), c(1))), eq(overflow, eq(c(0), c(1))));
    auto narrow = arith::TruncIOp::create(b, loc, b.getIntegerType(dst->bits), bits);
    FloatType type = dst->bits == 32 ? FloatType(b.getF32Type()) : FloatType(b.getF64Type());
    return {arith::BitcastOp::create(b, loc, type, narrow), valid};
  }
};
} // namespace
std::pair<mlir::Value, mlir::Value> numericConversion(mlir::OpBuilder &builder, mlir::Location loc,
                                                      mlir::Value input, NumericType source,
                                                      NumericType destination,
                                                      ArithmeticMode arithmetic) {
  // Every supported integer has at most 64 bits: both float formats cover its
  // entire finite range. Float32-to-float64 is also total. Avoid constructing
  // the software rounding/range machinery merely to prove these casts valid;
  // the target FPU still performs the one requested rounding operation.
  if (arithmetic != ArithmeticMode::Software && source != destination &&
      !isInteger(destination) &&
      (isInteger(source) || (source == NumericType::Float32 && destination == NumericType::Float64))) {
    auto valid = mlir::arith::ConstantIntOp::create(builder, loc, 1, 1);
    return {hardwareConversion(builder, loc, input, source, destination, valid), valid};
  }
  auto result = Converter(builder, loc).run(input, source, destination);
  if (arithmetic != ArithmeticMode::Software && source != destination &&
      !(isInteger(source) && isInteger(destination)))
    result.first = hardwareConversion(builder, loc, input, source, destination, result.second);
  return result;
}
} // namespace kalandra
