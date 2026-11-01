#include "kalandra/GpuInteger.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/Builders.h"

namespace kalandra {
namespace {
using namespace mlir;
struct Emitter {
  OpBuilder &b;
  Location loc;
  Value c(std::uint64_t x) {
    return arith::ConstantOp::create(b, loc, b.getI64Type(), b.getI64IntegerAttr(x));
  }
  Value add(Value a, Value d) { return arith::AddIOp::create(b, loc, a, d); }
  Value sub(Value a, Value d) { return arith::SubIOp::create(b, loc, a, d); }
  Value mul(Value a, Value d) { return arith::MulIOp::create(b, loc, a, d); }
  Value both(Value a, Value d) { return arith::AndIOp::create(b, loc, a, d); }
  Value either(Value a, Value d) { return arith::OrIOp::create(b, loc, a, d); }
  Value different(Value a, Value d) { return arith::XOrIOp::create(b, loc, a, d); }
  Value shl(Value a, unsigned n) { return arith::ShLIOp::create(b, loc, a, c(n)); }
  Value shr(Value a, unsigned n) { return arith::ShRUIOp::create(b, loc, a, c(n)); }
  Value cmp(arith::CmpIPredicate predicate, Value a, Value d) {
    return arith::CmpIOp::create(b, loc, predicate, a, d);
  }
  Value choose(Value condition, Value a, Value d) {
    return arith::SelectOp::create(b, loc, condition, a, d);
  }
  // Unsigned full product. Partial products and the middle-column carry fit
  // in 64 bits; the upper word is exact, including 0xffffffffffffffff squared.
  std::pair<Value, Value> product(Value a, Value d) {
    auto mask = c(UINT32_MAX);
    auto al = both(a, mask), ah = shr(a, 32), dl = both(d, mask), dh = shr(d, 32);
    auto ll = mul(al, dl), lh = mul(al, dh), hl = mul(ah, dl);
    auto middle = add(add(shr(ll, 32), both(lh, mask)), both(hl, mask));
    auto lower = either(both(ll, mask), shl(middle, 32));
    auto upper = add(add(add(mul(ah, dh), shr(lh, 32)), shr(hl, 32)), shr(middle, 32));
    return {lower, upper};
  }
};
} // namespace
std::optional<GpuIntegerResult> emitCheckedGpuInteger(OpBuilder &builder, Location loc,
                                                      NumericType type,
                                                      GpuIntegerOperation operation, Value left,
                                                      Value right) {
  if (!isInteger(type) || !left || !right || !left.getType().isInteger(64) ||
      !right.getType().isInteger(64))
    return {};
  using P = arith::CmpIPredicate;
  Emitter e{builder, loc};
  if (type == NumericType::UInt32 && operation == GpuIntegerOperation::Multiply) {
    // Native low/high uint32 multiplication makes overflow explicit. NVIDIA
    // 595.112.07 traps in its shader compiler on the redundant two-word uint64
    // expansion inside the balanced matrix reduction loop.
    auto a = arith::TruncIOp::create(builder, loc, builder.getI32Type(), left);
    auto d = arith::TruncIOp::create(builder, loc, builder.getI32Type(), right);
    auto product = arith::MulUIExtendedOp::create(builder, loc, a, d);
    auto zero = arith::ConstantIntOp::create(builder, loc, 0, 32);
    return GpuIntegerResult{
        arith::ExtUIOp::create(builder, loc, builder.getI64Type(), product.getLow()),
        arith::CmpIOp::create(builder, loc, P::eq, product.getHigh(), zero)};
  }
  const auto width = describeNumeric(type)->bits;
  const auto mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
  auto a = e.both(left, e.c(mask)), d = e.both(right, e.c(mask));
  const bool unsignedValue = isUnsigned(type);
  if (!unsignedValue && width < 64) {
    a = arith::ShRSIOp::create(builder, loc, e.shl(a, 64 - width), e.c(64 - width));
    d = arith::ShRSIOp::create(builder, loc, e.shl(d, 64 - width), e.c(64 - width));
  }
  auto zero = e.c(0);
  auto negativeA = e.cmp(P::slt, a, zero), negativeD = e.cmp(P::slt, d, zero);
  auto magnitudeA = unsignedValue ? a : e.choose(negativeA, e.sub(zero, a), a);
  auto magnitudeD = unsignedValue ? d : e.choose(negativeD, e.sub(zero, d), d);
  auto min = e.c(static_cast<std::uint64_t>(integerMinimum(type)));
  auto max = e.c(integerMaximum(type));
  Value value, valid;
  switch (operation) {
  case GpuIntegerOperation::Add:
  case GpuIntegerOperation::Subtract: {
    const bool add = operation == GpuIntegerOperation::Add;
    value = add ? e.add(a, d) : e.sub(a, d);
    if (unsignedValue)
      valid = add ? e.cmp(P::ule, a, e.sub(max, d)) : e.cmp(P::uge, a, d);
    else if (width < 64)
      valid = e.both(e.cmp(P::sge, value, min), e.cmp(P::sle, value, max));
    else {
      auto signChange = add ? e.both(e.different(a, value), e.different(d, value))
                            : e.both(e.different(a, d), e.different(a, value));
      valid = e.cmp(P::sge, signChange, zero);
    }
    break;
  }
  case GpuIntegerOperation::Multiply: {
    auto [lower, upper] = e.product(magnitudeA, magnitudeD);
    auto negative = e.different(negativeA, negativeD);
    auto limit = unsignedValue ? max : e.choose(negative, e.c(UINT64_C(1) << (width - 1)), max);
    valid = e.both(e.cmp(P::eq, upper, zero), e.cmp(P::ule, lower, limit));
    value = unsignedValue ? lower : e.choose(negative, e.sub(zero, lower), lower);
    break;
  }
  case GpuIntegerOperation::Divide:
  case GpuIntegerOperation::Remainder: {
    valid = e.cmp(P::ne, magnitudeD, zero);
    auto divisor = e.choose(valid, magnitudeD, e.c(1));
    const bool divide = operation == GpuIntegerOperation::Divide;
    Value magnitude = divide ? Value(arith::DivUIOp::create(builder, loc, magnitudeA, divisor))
                             : Value(arith::RemUIOp::create(builder, loc, magnitudeA, divisor));
    auto negative = divide ? e.different(negativeA, negativeD) : negativeA;
    if (!unsignedValue && divide) {
      auto limit = e.choose(negative, e.c(UINT64_C(1) << (width - 1)), max);
      valid = e.both(valid, e.cmp(P::ule, magnitude, limit));
    }
    value = unsignedValue ? magnitude : e.choose(negative, e.sub(zero, magnitude), magnitude);
    break;
  }
  case GpuIntegerOperation::Negate:
    valid = unsignedValue ? e.cmp(P::eq, a, zero) : e.cmp(P::ne, a, min);
    value = e.sub(zero, a);
    break;
  default:
    return {};
  }
  return GpuIntegerResult{e.both(value, e.c(mask)), valid};
}
} // namespace kalandra
