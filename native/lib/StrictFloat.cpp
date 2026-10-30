#include "kalandra/StrictFloat.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/TypeUtilities.h"

namespace kalandra {
namespace {
using namespace mlir;
template <unsigned Fraction, unsigned Exponent, bool Compact = false> class IntegerFloat {
  static constexpr unsigned Width = Fraction == 23 || Compact ? 64 : 128;
  static constexpr unsigned Bits = Fraction == 23 ? 32 : 64;
  static constexpr std::uint64_t Hidden = std::uint64_t{1} << Fraction;
  static constexpr std::uint64_t Sign = std::uint64_t{1} << (Bits - 1);
  static constexpr std::uint64_t Magnitude = Sign - 1;
  static constexpr std::uint64_t ExponentMask = (std::uint64_t{1} << Exponent) - 1;
  static constexpr std::uint64_t Infinity = ExponentMask << Fraction;
  static constexpr std::uint64_t Nan = Infinity | (Hidden >> 1);
  static constexpr unsigned Bias = (1u << (Exponent - 1)) - 1;
  OpBuilder b;
  Location loc;
  Value c(std::uint64_t n, unsigned width = Width) {
    return arith::ConstantOp::create(
        b, loc, b.getIntegerAttr(b.getIntegerType(width), llvm::APInt(width, n)));
  }
  Value signedC(std::int64_t n) {
    return arith::ConstantOp::create(
        b, loc, b.getIntegerAttr(b.getIntegerType(Width), llvm::APInt(Width, n, true)));
  }
  Value power(unsigned bit) {
    return arith::ConstantOp::create(
        b, loc, b.getIntegerAttr(b.getIntegerType(Width), llvm::APInt::getOneBitSet(Width, bit)));
  }
  FloatType floatType() {
    return Bits == 32 ? FloatType(b.getF32Type()) : FloatType(b.getF64Type());
  }
  Value add(Value x, Value y) { return arith::AddIOp::create(b, loc, x, y); }
  Value sub(Value x, Value y) { return arith::SubIOp::create(b, loc, x, y); }
  Value bitAnd(Value x, Value y) { return arith::AndIOp::create(b, loc, x, y); }
  Value bitOr(Value x, Value y) { return arith::OrIOp::create(b, loc, x, y); }
  Value shl(Value x, Value y) { return arith::ShLIOp::create(b, loc, x, y); }
  Value shr(Value x, Value y) { return arith::ShRUIOp::create(b, loc, x, y); }
  Value cmp(arith::CmpIPredicate p, Value x, Value y) {
    return arith::CmpIOp::create(b, loc, p, x, y);
  }
  Value eq(Value x, Value y) { return cmp(arith::CmpIPredicate::eq, x, y); }
  Value lt(Value x, Value y) { return cmp(arith::CmpIPredicate::ult, x, y); }
  Value slt(Value x, Value y) { return cmp(arith::CmpIPredicate::slt, x, y); }
  Value choose(Value p, Value x, Value y) { return arith::SelectOp::create(b, loc, p, x, y); }
  Value all(Value x, Value y) { return bitAnd(x, y); }
  Value wide(Value x) {
    if constexpr (Width == Bits)
      return x;
    return arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), x);
  }
  Value narrow(Value x) {
    if constexpr (Width == Bits)
      return x;
    return arith::TruncIOp::create(b, loc, b.getIntegerType(Bits), x);
  }
  Value nonzero(Value x) { return cmp(arith::CmpIPredicate::ne, x, c(0)); }
  // Every emitted shift stays in 1..Width-1, including eagerly evaluated select arms.
  // Bit zero jams discarded bits; count >= Width reduces any nonzero value to 1.
  Value jam(Value x, Value count) {
    auto safe = choose(lt(count, c(1)), c(1), choose(lt(count, c(Width)), count, c(Width - 1)));
    auto lost = nonzero(shl(x, sub(c(Width), safe)));
    auto sticky = arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), lost);
    auto shifted = bitOr(shr(x, safe), sticky);
    auto any = arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), nonzero(x));
    return choose(eq(count, c(0)), x, choose(lt(count, c(Width)), shifted, any));
  }
  struct Parts {
    Value bits, sign, exponent, significand, isNan, isInf, isZero;
  };
  Parts unpack(Value value) {
    auto bits = wide(arith::BitcastOp::create(b, loc, b.getIntegerType(Bits), value));
    auto sign = bitAnd(bits, c(Sign));
    auto magnitude = bitAnd(bits, c(Magnitude));
    auto exponent = bitAnd(shr(bits, c(Fraction)), c(ExponentMask));
    auto fraction = bitAnd(bits, c(Hidden - 1));
    return {bits,
            sign,
            choose(eq(exponent, c(0)), c(1), exponent),
            choose(eq(exponent, c(0)), fraction, bitOr(fraction, c(Hidden))),
            lt(c(Infinity), magnitude),
            eq(magnitude, c(Infinity)),
            eq(magnitude, c(0))};
  }
  // Normalize in logarithmic steps. A shift is allowed only if it cannot
  // overshoot the target bit or cross the subnormal exponent floor. Zero
  // keeps its exponent, just as in the original bounded one-bit loop.
  std::pair<Value, Value> normalize(Value sig, Value exponent, unsigned bit, std::int64_t floor) {
    for (unsigned shift = Bits == 32 ? 16 : 32; shift != 0; shift >>= 1) {
      if (shift > bit)
        continue;
      auto fits = lt(sig, power(bit - shift + 1));
      auto room = cmp(arith::CmpIPredicate::sge, exponent, signedC(floor + shift));
      auto move = all(all(nonzero(sig), fits), room);
      sig = choose(move, shl(sig, c(shift)), sig);
      exponent = choose(move, sub(exponent, c(shift)), exponent);
    }
    return {sig, exponent};
  }
  Value pack(Value sig, Value exponent, Value sign) {
    // Three low bits are guard/round/sticky. The retained odd bit breaks ties.
    auto main = shr(sig, c(3));
    auto tail = bitAnd(sig, c(7));
    auto increment = bitOr(lt(c(4), tail), all(eq(tail, c(4)), nonzero(bitAnd(main, c(1)))));
    auto rounded = add(main, arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), increment));
    auto carry = eq(rounded, c(Hidden << 1));
    rounded = choose(carry, shr(rounded, c(1)), rounded);
    exponent = choose(carry, add(exponent, c(1)), exponent);
    auto field = choose(lt(rounded, c(Hidden)), c(0), exponent);
    auto bits = bitOr(sign, bitOr(shl(field, c(Fraction)), bitAnd(rounded, c(Hidden - 1))));
    return choose(slt(exponent, c(ExponentMask)), bits, bitOr(sign, c(Infinity)));
  }
  Value addition(Parts a, Parts d) {
    auto magnitudeA = bitAnd(a.bits, c(Magnitude)), magnitudeB = bitAnd(d.bits, c(Magnitude));
    auto swap = lt(magnitudeA, magnitudeB);
    auto sigA = choose(swap, d.significand, a.significand),
         sigB = choose(swap, a.significand, d.significand);
    auto expA = choose(swap, d.exponent, a.exponent), expB = choose(swap, a.exponent, d.exponent);
    auto sign = choose(swap, d.sign, a.sign);
    auto same = eq(a.sign, d.sign);
    sigA = shl(sigA, c(3));
    sigB = jam(shl(sigB, c(3)), sub(expA, expB));
    auto sig = choose(same, add(sigA, sigB), sub(sigA, sigB));
    auto carry = cmp(arith::CmpIPredicate::uge, sig, c(Hidden << 4));
    sig = choose(carry, jam(sig, c(1)), sig);
    expA = choose(carry, add(expA, c(1)), expA);
    auto [normal, exponent] = normalize(sig, expA, Fraction + 3, 1);
    auto zeroSign = bitAnd(a.sign, d.sign);
    auto result = choose(eq(normal, c(0)), zeroSign, pack(normal, exponent, sign));
    result = choose(d.isInf, d.bits, result);
    result = choose(a.isInf, a.bits, result);
    auto invalid = all(all(a.isInf, d.isInf), cmp(arith::CmpIPredicate::ne, a.sign, d.sign));
    return choose(bitOr(bitOr(a.isNan, d.isNan), invalid), c(Nan), result);
  }
  Value multiplication(Parts a, Parts d) {
    auto [sigA, expA] = normalize(a.significand, a.exponent, Fraction, 1 - std::int64_t(Fraction));
    auto [sigB, expB] = normalize(d.significand, d.exponent, Fraction, 1 - std::int64_t(Fraction));
    Value high, sig;
    if constexpr (Compact && Fraction == 52) {
      // Exact 53x53 product, represented by two 64-bit limbs. Every partial
      // product and the middle-column carry fits in an unsigned 64-bit word.
      auto mask = c(UINT32_MAX);
      auto loA = bitAnd(sigA, mask), hiA = shr(sigA, c(32));
      auto loB = bitAnd(sigB, mask), hiB = shr(sigB, c(32));
      auto mul = [&](Value x, Value y) -> Value { return arith::MulIOp::create(b, loc, x, y); };
      auto p00 = mul(loA, loB), p01 = mul(loA, hiB), p10 = mul(hiA, loB);
      auto middle = add(add(shr(p00, c(32)), bitAnd(p01, mask)), bitAnd(p10, mask));
      auto lower = bitOr(bitAnd(p00, mask), shl(middle, c(32)));
      auto upper =
          add(add(add(mul(hiA, hiB), shr(p01, c(32))), shr(p10, c(32))), shr(middle, c(32)));
      high = nonzero(bitAnd(upper, power(2 * Fraction + 1 - 64)));
      auto shift = choose(high, c(Fraction - 2), c(Fraction - 3));
      auto sticky = arith::ExtUIOp::create(b, loc, b.getIntegerType(Width),
                                           nonzero(shl(lower, sub(c(64), shift))));
      sig = bitOr(bitOr(shr(lower, shift), shl(upper, sub(c(64), shift))), sticky);
    } else {
      auto product = arith::MulIOp::create(b, loc, sigA, sigB);
      high = nonzero(bitAnd(product, power(2 * Fraction + 1)));
      sig = jam(product, choose(high, c(Fraction - 2), c(Fraction - 3)));
    }
    auto exponent = sub(add(expA, expB), c(Bias));
    exponent = choose(high, add(exponent, c(1)), exponent);
    auto tiny = slt(exponent, c(1));
    sig = choose(tiny, jam(sig, sub(c(1), exponent)), sig);
    exponent = choose(tiny, c(1), exponent);
    auto sign = arith::XOrIOp::create(b, loc, a.sign, d.sign);
    auto result = pack(sig, exponent, sign);
    result = choose(bitOr(a.isZero, d.isZero), sign, result);
    result = choose(bitOr(a.isInf, d.isInf), bitOr(sign, c(Infinity)), result);
    auto invalid = bitOr(all(a.isInf, d.isZero), all(d.isInf, a.isZero));
    return choose(bitOr(bitOr(a.isNan, d.isNan), invalid), c(Nan), result);
  }

  Value division(Parts a, Parts d) {
    auto [sigA, expA] = normalize(a.significand, a.exponent, Fraction, 1 - std::int64_t(Fraction));
    auto [sigB, expB] = normalize(d.significand, d.exponent, Fraction, 1 - std::int64_t(Fraction));
    // Normalize the ratio to [1,2), retaining guard/round/sticky bits.
    // The shifted numerator fits in 2*Fraction+5 bits. Protect the integer divisor even
    // for special operands: select arms may be evaluated eagerly.
    auto belowOne = lt(sigA, sigB);
    auto divisor = choose(nonzero(sigB), sigB, c(1));
    Value quotient, remainder;
    if constexpr (Compact && Fraction == 52) {
      // Restoring division produces the 56 significant/rounding bits directly;
      // neither a 109-bit numerator nor native floating-point division is needed.
      auto numerator = choose(belowOne, shl(sigA, c(1)), sigA);
      auto leading = cmp(arith::CmpIPredicate::uge, numerator, divisor);
      quotient = arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), leading);
      remainder = choose(leading, sub(numerator, divisor), numerator);
      for (unsigned bit = 0; bit < Fraction + 3; ++bit) {
        auto shifted = shl(remainder, c(1));
        auto take = cmp(arith::CmpIPredicate::uge, shifted, divisor);
        remainder = choose(take, sub(shifted, divisor), shifted);
        quotient = bitOr(shl(quotient, c(1)),
                         arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), take));
      }
    } else {
      auto numerator = shl(sigA, choose(belowOne, c(Fraction + 4), c(Fraction + 3)));
      quotient = arith::DivUIOp::create(b, loc, numerator, divisor);
      remainder = arith::RemUIOp::create(b, loc, numerator, divisor);
    }
    auto sticky = arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), nonzero(remainder));
    auto sig = bitOr(quotient, sticky);
    auto exponent = sub(add(sub(expA, expB), c(Bias)),
                        arith::ExtUIOp::create(b, loc, b.getIntegerType(Width), belowOne));
    auto tiny = slt(exponent, c(1));
    sig = choose(tiny, jam(sig, sub(c(1), exponent)), sig);
    exponent = choose(tiny, c(1), exponent);
    auto sign = arith::XOrIOp::create(b, loc, a.sign, d.sign);
    auto result = pack(sig, exponent, sign);
    result = choose(bitOr(a.isZero, d.isInf), sign, result);
    result = choose(bitOr(a.isInf, d.isZero), bitOr(sign, c(Infinity)), result);
    auto invalid = bitOr(all(a.isZero, d.isZero), all(a.isInf, d.isInf));
    return choose(bitOr(bitOr(a.isNan, d.isNan), invalid), c(Nan), result);
  }

public:
  explicit IntegerFloat(Operation *op) : b(op), loc(op->getLoc()) {}
  Value lower(Operation *op) {
    auto flipSign = [&](Value value) -> Value {
      auto bits = arith::BitcastOp::create(b, loc, b.getIntegerType(Bits), value);
      auto flipped = arith::XOrIOp::create(b, loc, bits, c(Sign, Bits));
      return arith::BitcastOp::create(b, loc, floatType(), flipped);
    };
    // Negation changes only the sign bit, including zero and NaN payloads.
    if (isa<arith::NegFOp>(op))
      return flipSign(op->getOperand(0));
    auto right = op->getOperand(1);
    if (isa<arith::SubFOp>(op))
      right = flipSign(right);
    auto a = unpack(op->getOperand(0)), d = unpack(right);
    auto bits = isa<arith::DivFOp>(op)   ? division(a, d)
                : isa<arith::MulFOp>(op) ? multiplication(a, d)
                                         : addition(a, d);
    return arith::BitcastOp::create(b, loc, floatType(), narrow(bits));
  }
};
} // namespace
namespace {
mlir::LogicalResult lowerStrictFloat(mlir::Operation *root, bool cpu, bool compact = false) {
  root->getContext()->getOrLoadDialect<mlir::scf::SCFDialect>();
  llvm::SmallVector<mlir::Operation *> operations;
  bool valid = true;
  root->walk([&](mlir::Operation *op) {
    if (cpu && (op->getName().getDialectNamespace() == "gpu" ||
                op->getName().getDialectNamespace() == "spirv")) {
      op->emitError("strict CPU floating lowering does not accept GPU/SPIR-V operations");
      valid = false;
    }
  });
  root->walk([&](mlir::Operation *op) {
    if (op->getName().getDialectNamespace() != "arith")
      return;
    if (mlir::isa<mlir::arith::BitcastOp>(op))
      return;
    bool floating = false;
    for (auto type : op->getOperandTypes())
      floating |= mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(type));
    for (auto type : op->getResultTypes())
      floating |= mlir::isa<mlir::FloatType>(mlir::getElementTypeOrSelf(type));
    if (!floating)
      return;
    if (mlir::isa<mlir::arith::AddFOp, mlir::arith::SubFOp, mlir::arith::MulFOp,
                  mlir::arith::NegFOp, mlir::arith::DivFOp>(op) &&
        (op->getResult(0).getType().isF32() ||
         ((cpu || compact) && op->getResult(0).getType().isF64())))
      operations.push_back(op);
    else {
      op->emitError(cpu ? "strict CPU floating lowering supports only scalar float32/float64 "
                          "add/subtract/multiply/divide/negate"
                        : "strict binary32 lowering supports only scalar float32 "
                          "add/subtract/multiply/divide/negate");
      valid = false;
    }
  });
  if (!valid)
    return mlir::failure();
  for (auto *op : operations) {
    auto result = op->getResult(0).getType().isF32() ? IntegerFloat<23, 8>(op).lower(op)
                  : compact                          ? IntegerFloat<52, 11, true>(op).lower(op)
                                                     : IntegerFloat<52, 11>(op).lower(op);
    op->getResult(0).replaceAllUsesWith(result);
    op->erase();
  }
  return mlir::success();
}
} // namespace
mlir::LogicalResult lowerStrictFloat32(mlir::Operation *root) {
  return lowerStrictFloat(root, false);
}
mlir::LogicalResult lowerStrictGpuFloat64(mlir::Operation *root) {
  return lowerStrictFloat(root, false, true);
}
mlir::LogicalResult lowerStrictCpuFloat(mlir::Operation *root) {
  return lowerStrictFloat(root, true);
}
mlir::LogicalResult lowerOutlinedStrictCpuFloat(mlir::Operation *operation) {
  using namespace mlir;
  auto module = operation->getParentOfType<ModuleOp>();
  if (!module || operation->getNumResults() != 1)
    return failure();
  auto suffix = operation->getName().getStringRef().drop_front(6);
  std::string name = "kalandra_software_" +
                     std::string(operation->getResult(0).getType().isF64() ? "f64_" : "f32_") +
                     suffix.str();
  auto helper = module.lookupSymbol<func::FuncOp>(name);
  if (!helper) {
    OpBuilder builder(module.getContext());
    builder.setInsertionPointToEnd(module.getBody());
    helper = func::FuncOp::create(
        builder, operation->getLoc(), name,
        builder.getFunctionType(operation->getOperandTypes(), operation->getResultTypes()));
    helper.setPrivate();
    builder.setInsertionPointToStart(helper.addEntryBlock());
    OperationState state(operation->getLoc(), operation->getName());
    state.addOperands(helper.getArguments());
    state.addTypes(operation->getResultTypes());
    auto *arithmetic = builder.create(state);
    func::ReturnOp::create(builder, operation->getLoc(), arithmetic->getResults());
    if (failed(lowerStrictCpuFloat(helper)))
      return failure();
  }
  OpBuilder builder(operation);
  auto call = func::CallOp::create(builder, operation->getLoc(), helper, operation->getOperands());
  operation->getResult(0).replaceAllUsesWith(call.getResult(0));
  operation->erase();
  return success();
}
mlir::LogicalResult lowerMetalHardwareFloat(mlir::Operation *root, bool allowSoftwareFallback) {
  llvm::SmallVector<mlir::Operation *> doubles;
  root->walk([&](mlir::Operation *op) {
    if (mlir::isa<mlir::arith::AddFOp, mlir::arith::SubFOp, mlir::arith::MulFOp,
                  mlir::arith::DivFOp, mlir::arith::NegFOp>(op) &&
        op->getResult(0).getType().isF64())
      doubles.push_back(op);
  });
  if (!allowSoftwareFallback && !doubles.empty()) {
    doubles.front()->emitError("Metal has no hardware float64 arithmetic; explicitly select "
                               "--arithmetic hardware-with-software-fallback or software");
    return mlir::failure();
  }
  for (auto *op : doubles) {
    auto result = IntegerFloat<52, 11, true>(op).lower(op);
    op->getResult(0).replaceAllUsesWith(result);
    op->erase();
  }
  return mlir::success();
}
} // namespace kalandra
