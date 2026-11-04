#include "DeviceHeap.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
using namespace mlir;
namespace kalandra {
void DeviceHeapEmitter::beginSort(Value handle, Value count) {
  store(SortHandle, handle);
  store(SortBuild, arith::DivUIOp::create(b, loc, count, c(2)));
  store(SortEnd, count);
  store(SortRoot, c(0));
  store(SortResume, c(1));
  store(SortPhase, c(1));
}
Value DeviceHeapEmitter::sortStep(NumericType element, bool boolean) {
  const auto compare = [&](arith::CmpIPredicate pred, Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, pred, a, d);
  };
  const auto when = [&](Value condition, auto body) {
    auto branch = scf::IfOp::create(b, loc, condition, false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    body();
  };
  const auto choose = [&](Value condition, auto left, auto right) {
    auto branch = scf::IfOp::create(b, loc, condition, true);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    left();
    b.setInsertionPointToStart(&branch.getElseRegion().front());
    right();
  };
  const auto handle = load(SortHandle), phase = load(SortPhase), end = load(SortEnd);
  const auto item = [&](Value index) { return read(handle, add(c(4), index)); };
  const auto less = [&](Value a, Value d) -> Value {
    const auto description = describeNumeric(element);
    const auto bits = boolean ? 1u : description->bits;
    const auto type = b.getIntegerType(bits);
    if (bits < 64) {
      a = arith::TruncIOp::create(b, loc, type, a);
      d = arith::TruncIOp::create(b, loc, type, d);
    }
    const bool floating = !boolean && description->family == NumericFamily::BinaryFloat;
    if (floating) {
      const auto key = [&](Value raw) -> Value {
        const Value zero = arith::ConstantIntOp::create(b, loc, 0, bits);
        const Value sign = arith::ConstantOp::create(
            b, loc, type, b.getIntegerAttr(type, llvm::APInt::getSignMask(bits)));
        const Value all = arith::ConstantOp::create(
            b, loc, type, b.getIntegerAttr(type, llvm::APInt::getAllOnes(bits)));
        const Value mask = arith::SelectOp::create(
            b, loc, compare(arith::CmpIPredicate::slt, raw, zero), all, sign);
        return arith::XOrIOp::create(b, loc, raw, mask);
      };
      a = key(a);
      d = key(d);
    }
    // Metal/SPIR-V legalization does not support every narrow unsigned cmp.
    // Extend the exact-width key, preserving signed integer ordering.
    if (bits < 32) {
      if (!boolean && description->family == NumericFamily::SignedInteger) {
        a = arith::ExtSIOp::create(b, loc, b.getI32Type(), a);
        d = arith::ExtSIOp::create(b, loc, b.getI32Type(), d);
      } else {
        a = arith::ExtUIOp::create(b, loc, b.getI32Type(), a);
        d = arith::ExtUIOp::create(b, loc, b.getI32Type(), d);
      }
    }
    return compare(!boolean && description->family == NumericFamily::SignedInteger
                       ? arith::CmpIPredicate::slt
                       : arith::CmpIPredicate::ult,
                   a, d);
  };
  const auto swap = [&](Value a, Value d) {
    const auto av = item(a), dv = item(d);
    // Both stores belong to this source step and submission. Cancellation never
    // observes one half of a swap or releases either backing before its fence.
    write(handle, add(c(4), a), dv);
    write(handle, add(c(4), d), av);
  };
  auto valid = compare(arith::CmpIPredicate::uge, phase, c(1));
  valid = arith::AndIOp::create(b, loc, valid, compare(arith::CmpIPredicate::ule, phase, c(3)));
  when(arith::XOrIOp::create(b, loc, valid, arith::ConstantIntOp::create(b, loc, 1, 1)),
       [&] { fault(99); });
  const auto resume = load(SortResume);
  const Value validResume = arith::OrIOp::create(b, loc, eq(resume, c(1)), eq(resume, c(3)));
  when(arith::AndIOp::create(
           b, loc, eq(phase, c(2)),
           arith::XOrIOp::create(b, loc, validResume, arith::ConstantIntOp::create(b, loc, 1, 1))),
       [&] { fault(99); });
  // Build one root, then return here after its bounded sift finishes.
  when(eq(phase, c(1)), [&] {
    const auto build = load(SortBuild);
    choose(
        compare(arith::CmpIPredicate::ugt, build, c(0)),
        [&] {
          const Value root = arith::SubIOp::create(b, loc, build, c(1));
          store(SortBuild, root);
          store(SortRoot, root);
          store(SortResume, c(1));
          store(SortPhase, c(2));
        },
        [&] { store(SortPhase, c(3)); });
  });
  when(eq(phase, c(2)), [&] {
    const auto root = load(SortRoot);
    const auto parent =
        compare(arith::CmpIPredicate::ult, root, arith::DivUIOp::create(b, loc, end, c(2)));
    choose(
        parent,
        [&] {
          const auto child = add(arith::MulIOp::create(b, loc, root, c(2)), c(1));
          const auto right = add(child, c(1));
          auto selected = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()},
                                            compare(arith::CmpIPredicate::ult, right, end), true);
          {
            OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(&selected.getThenRegion().front());
            const Value pick =
                arith::SelectOp::create(b, loc, less(item(child), item(right)), right, child);
            scf::YieldOp::create(b, loc, ValueRange{pick});
            b.setInsertionPointToStart(&selected.getElseRegion().front());
            scf::YieldOp::create(b, loc, ValueRange{child});
          }
          const auto next = selected.getResult(0);
          choose(
              less(item(root), item(next)),
              [&] {
                swap(root, next);
                store(SortRoot, next);
              },
              [&] { store(SortPhase, load(SortResume)); });
        },
        [&] { store(SortPhase, load(SortResume)); });
  });
  const Value finished =
      arith::AndIOp::create(b, loc, eq(phase, c(3)), compare(arith::CmpIPredicate::ule, end, c(1)));
  when(eq(phase, c(3)), [&] {
    choose(
        compare(arith::CmpIPredicate::ugt, end, c(1)),
        [&] {
          const Value last = arith::SubIOp::create(b, loc, end, c(1));
          swap(c(0), last);
          store(SortEnd, last);
          store(SortRoot, c(0));
          store(SortResume, c(3));
          store(SortPhase, c(2));
        },
        [&] { store(SortPhase, c(0)); });
  });
  return finished;
}
} // namespace kalandra
