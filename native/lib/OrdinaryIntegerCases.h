#pragma once
#include "OrdinaryScalarRanges.h"
#include "OrdinaryStableLoops.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// Prove nonnegativity over an SSA recurrence: every initial edge must prove it,
// and every cycle operation must preserve it on defined executions.
inline bool ordinaryNonnegative(llvm::Value *value, llvm::SmallPtrSetImpl<llvm::Value *> &path) {
  using namespace llvm;
  if (auto *constant = dyn_cast<ConstantInt>(value))
    return !constant->isNegative();
  if (auto *argument = dyn_cast<Argument>(value)) {
    auto range = argument->getRange();
    return range && !range->isFullSet() && !range->getUnsignedMax().isNegative();
  }
  if (!path.insert(value).second)
    return true;
  auto next = [&](Value *v) { return ordinaryNonnegative(v, path); };
  bool good = false;
  if (auto *phi = dyn_cast<PHINode>(value)) {
    good = true;
    for (auto &v : phi->incoming_values())
      good &= next(v);
  } else if (auto *select = dyn_cast<SelectInst>(value))
    good = next(select->getTrueValue()) && next(select->getFalseValue());
  else if (isa<ZExtInst>(value))
    good = true;
  else if (auto *cast = dyn_cast<SExtInst>(value))
    good = next(cast->getOperand(0));
  else if (auto *cast = dyn_cast<TruncInst>(value))
    good = cast->hasNoSignedWrap() && next(cast->getOperand(0));
  else if (auto *op = dyn_cast<BinaryOperator>(value)) {
    if (op->getOpcode() == Instruction::SRem || op->getOpcode() == Instruction::URem)
      good = next(op->getOperand(0)) && next(op->getOperand(1));
    else if ((op->getOpcode() == Instruction::Add || op->getOpcode() == Instruction::Mul) &&
             op->hasNoSignedWrap())
      good = next(op->getOperand(0)) && next(op->getOperand(1));
    else if (op->getOpcode() == Instruction::And)
      if (auto *mask = dyn_cast<ConstantInt>(op->getOperand(1)))
        good = !mask->isNegative();
  }
  path.erase(value);
  return good;
}
inline bool
specializeOrdinaryIntegerCases(llvm::Module &module,
                               const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.direct") &&
        !fn.hasFnAttribute("kalandra.ordinary.integer-cases") &&
        fn.getName().ends_with("_ordinary_direct_v1") && fn.arg_size() && fn.arg_size() <= 4)
      roots.push_back(&fn);
  bool changed = false;
  for (auto *original : roots) {
    bool eligible = true;
    for (auto &arg : original->args())
      eligible &= arg.getType()->isIntegerTy(64);
    if (!eligible || original->getInstructionCount() > 1024)
      continue;
    DominatorTree dt(*original);
    LoopInfo loops(dt);
    Argument *counter = nullptr;
    for (auto *loop : loops.getLoopsInPreorder())
      for (auto &phi : loop->getHeader()->phis()) {
        if (!phi.getType()->isIntegerTy(64) || phi.getNumIncomingValues() != 2)
          continue;
        for (unsigned k = 0; k < 2; ++k) {
          auto *arg = dyn_cast<Argument>(phi.getIncomingValue(k));
          auto *step = dyn_cast<BinaryOperator>(phi.getIncomingValue(1 - k));
          if (arg && step && step->getOpcode() == Instruction::Add && step->getOperand(0) == &phi)
            if (auto *amount = dyn_cast<ConstantInt>(step->getOperand(1));
                amount && amount->isMinusOne())
              counter = arg;
        }
      }
    if (!counter)
      continue;
    bool hasRemainder = false;
    for (auto &block : *original)
      for (auto &i : block)
        hasRemainder |= i.getOpcode() == Instruction::SRem || i.getOpcode() == Instruction::URem;
    if (!hasRemainder)
      continue;
    const unsigned count = counter->getArgNo();
    ValueToValueMapTy map;
    auto *bounded = CloneFunction(original, map);
    bounded->setLinkage(GlobalValue::InternalLinkage);
    bounded->setName(original->getName() + ".small-integer-candidate");
    for (auto &arg : bounded->args())
      bounded->addParamAttr(
          arg.getArgNo(),
          Attribute::get(
              module.getContext(), Attribute::Range,
              ConstantRange(APInt(64, arg.getArgNo() == count ? 1 : 0),
                            arg.getArgNo() == count ? APInt::getSignMask(64) : APInt(64, 65536))));
    simplify(*bounded, true);
    {
      DominatorTree peelDom(*bounded);
      LoopInfo peelLoops(peelDom);
      AssumptionCache ac(*bounded);
      TargetLibraryInfoImpl impl(module.getTargetTriple());
      TargetLibraryInfo tli(impl);
      ScalarEvolution se(*bounded, tli, ac, peelDom, peelLoops);
      for (auto *loop : peelLoops.getLoopsInPreorder())
        if (loop->getSubLoops().empty() && canPeel(loop)) {
          formLCSSARecursively(*loop, peelDom, &peelLoops, &se);
          ValueToValueMapTy peeled;
          peelLoop(loop, 1, false, &peelLoops, &se, peelDom, &ac, true, peeled);
          break;
        }
    }
    if (verifyFunction(*bounded)) {
      bounded->eraseFromParent();
      continue;
    }
    simplify(*bounded, true);
    SmallVector<BinaryOperator *, 8> positiveRemainders;
    for (auto &block : *bounded)
      for (auto &i : block)
        if (i.getOpcode() == Instruction::SRem) {
          auto *rem = cast<BinaryOperator>(&i);
          SmallPtrSet<Value *, 32> path;
          if (ordinaryNonnegative(rem->getOperand(0), path) &&
              ordinaryNonnegative(rem->getOperand(1), path))
            positiveRemainders.push_back(rem);
        }
    for (auto *rem : positiveRemainders) {
      IRBuilder<> at(rem);
      rem->replaceAllUsesWith(at.CreateURem(rem->getOperand(0), rem->getOperand(1)));
      rem->eraseFromParent();
    }
    simplify(*bounded, true);
    DominatorTree dom(*bounded);
    LoopInfo inner(dom);
    SmallVector<BinaryOperator *, 8> remainders;
    for (auto &block : *bounded)
      for (auto &i : block)
        if (i.getOpcode() == Instruction::SRem || i.getOpcode() == Instruction::URem)
          remainders.push_back(cast<BinaryOperator>(&i));
    unsigned narrowed = 0;
    for (auto *rem : remainders) {
      SmallPtrSet<Value *, 32> path;
      if (!ordinaryNonnegative(rem->getOperand(0), path) ||
          !ordinaryNonnegative(rem->getOperand(1), path))
        continue;
      auto *loop = inner.getLoopFor(rem->getParent());
      if (!loop)
        continue;
      PHINode *divisor = nullptr;
      ConstantRange initial = ConstantRange::getFull(64);
      for (auto &phi : loop->getHeader()->phis())
        if (phi.getType()->isIntegerTy(64) && phi.getNumIncomingValues() == 2)
          for (unsigned k = 0; k < 2; ++k)
            if (phi.getIncomingValue(k) == rem && !loop->contains(phi.getIncomingBlock(1 - k))) {
              Value *rhs = rem->getOperand(1);
              bool same = rhs == &phi;
              if (auto *select = dyn_cast<SelectInst>(rhs)) {
                auto *cmp = dyn_cast<ICmpInst>(select->getCondition());
                auto *oneValue = dyn_cast<ConstantInt>(select->getTrueValue());
                if (cmp && cmp->getPredicate() == ICmpInst::ICMP_EQ && cmp->getOperand(0) == &phi &&
                    select->getFalseValue() == &phi && oneValue && oneValue->isOne())
                  if (auto *minusOne = dyn_cast<ConstantInt>(cmp->getOperand(1)))
                    same = minusOne->isMinusOne();
              }
              if (same) {
                divisor = &phi;
                initial = computeConstantRangeIncludingKnownBits(
                    WithCache<const Value *>(phi.getIncomingValue(1 - k)), false,
                    SimplifyQuery(module.getDataLayout()));
              }
            }
      if (!divisor || initial.isFullSet() || initial.isEmptySet() ||
          initial.getUnsignedMax().getActiveBits() > 32)
        continue;
      // For the Euclidean pair (a,b) -> (b,a%b), b never grows. If both
      // initial edges fit u32, neither recurrence can acquire high bits.
      // Express that proof with narrow arithmetic rather than an identity
      // mask that InstCombine may discard before narrowing the cyclic phis.
      auto *dividend = dyn_cast<PHINode>(rem->getOperand(0));
      bool boundedDividend = false;
      if (dividend && dividend->getParent() == loop->getHeader() &&
          dividend->getNumIncomingValues() == 2)
        for (unsigned k = 0; k < 2; ++k)
          if (loop->contains(dividend->getIncomingBlock(k)) &&
              dividend->getIncomingValue(k) == divisor &&
              !loop->contains(dividend->getIncomingBlock(1 - k))) {
            auto a = computeConstantRangeIncludingKnownBits(
                WithCache<const Value *>(dividend->getIncomingValue(1 - k)), false,
                SimplifyQuery(module.getDataLayout()));
            boundedDividend =
                !a.isFullSet() && !a.isEmptySet() && a.getUnsignedMax().getActiveBits() <= 32;
          }
      if (boundedDividend) {
        IRBuilder<> at(rem);
        auto *value = at.CreateURem(at.CreateTrunc(rem->getOperand(0), at.getInt32Ty()),
                                    at.CreateTrunc(rem->getOperand(1), at.getInt32Ty()));
        const unsigned bits = std::max(1u, initial.getUnsignedMax().getActiveBits());
        value =
            at.CreateAnd(value, ConstantInt::get(at.getInt32Ty(), APInt::getLowBitsSet(32, bits)));
        rem->replaceAllUsesWith(at.CreateZExt(value, rem->getType()));
        rem->eraseFromParent();
        ++narrowed;
        continue;
      }
      const unsigned bits = std::max(1u, initial.getUnsignedMax().getActiveBits());
      IRBuilder<> b(&*loop->getHeader()->getFirstInsertionPt());
      auto *bounded = cast<Instruction>(
          b.CreateAnd(divisor, ConstantInt::get(divisor->getType(), APInt::getLowBitsSet(64, bits)),
                      "ordinary.euclid.bound"));
      // For nonnegative a,b, remainder(a,b) is in [0,b). This recurrence
      // never exceeds its initial bound; clearing high bits is an identity.
      divisor->replaceAllUsesWith(bounded);
      bounded->setOperand(0, divisor);
      IRBuilder<> at(rem);
      auto *positive = at.CreateURem(rem->getOperand(0), rem->getOperand(1));
      rem->replaceAllUsesWith(positive);
      rem->eraseFromParent();
      ++narrowed;
    }
    if (!narrowed || bounded->getInstructionCount() > 256) {
      bounded->eraseFromParent();
      continue;
    }
    simplify(*bounded, true);
    if (verifyFunction(*bounded) || nativeStackBudgetError(*bounded, module.getDataLayout())) {
      bounded->eraseFromParent();
      continue;
    }
    const bool stable = accelerateOrdinaryStableLoop(*bounded);
    if (stable)
      simplify(*bounded, true);
    ValueToValueMapTy zeroMap;
    auto *zero = CloneFunction(original, zeroMap);
    zero->setLinkage(GlobalValue::InternalLinkage);
    zero->getArg(count)->replaceAllUsesWith(ConstantInt::get(counter->getType(), 0));
    simplify(*zero, true);
    bool pure = zero->size() == 1;
    for (auto &block : *zero)
      for (auto &i : block)
        if (i.mayHaveSideEffects() || i.mayReadFromMemory() || isa<CallBase>(i) ||
            isa<AllocaInst>(i))
          pure = false;
    if (!pure || zero->getInstructionCount() > 24) {
      zero->eraseFromParent();
      bounded->eraseFromParent();
      continue;
    }
    auto &ctx = module.getContext();
    auto *wrapper = llvm::Function::Create(original->getFunctionType(), original->getLinkage(),
                                           original->getName() + ".small-wrapper", module);
    wrapper->copyAttributesFrom(original);
    auto *entry = BasicBlock::Create(ctx, "ordinary.integer.entry", wrapper);
    auto *base = BasicBlock::Create(ctx, "ordinary.integer.zero", wrapper);
    auto *guard = BasicBlock::Create(ctx, "ordinary.integer.guard", wrapper);
    auto *fast = BasicBlock::Create(ctx, "ordinary.integer.bounded", wrapper);
    auto *wide = BasicBlock::Create(ctx, "ordinary.integer.wide", wrapper);
    IRBuilder<> b(entry);
    b.CreateCondBr(b.CreateICmpEQ(wrapper->getArg(count), b.getInt64(0)), base, guard);
    SmallVector<Value *, 8> args;
    for (auto &arg : wrapper->args())
      args.push_back(&arg);
    b.SetInsertPoint(base);
    auto *baseCall = b.CreateCall(zero, args);
    b.CreateRet(baseCall);
    InlineFunctionInfo baseInfo;
    if (!InlineFunction(*baseCall, baseInfo).isSuccess()) {
      wrapper->eraseFromParent();
      zero->eraseFromParent();
      bounded->eraseFromParent();
      continue;
    }
    zero->eraseFromParent();
    b.SetInsertPoint(guard);
    Value *invalid = b.getInt64(0);
    for (auto &arg : wrapper->args())
      invalid = b.CreateOr(invalid, b.CreateLShr(&arg, arg.getArgNo() == count ? 63 : 16));
    // Keep the compact success leaf on the fall-through path. A taken
    // dispatch branch costs as much as the arithmetic in very short entries.
    b.CreateCondBr(b.CreateICmpEQ(invalid, b.getInt64(0)), fast, wide,
                   MDBuilder(ctx).createBranchWeights(2, 1));
    b.SetInsertPoint(fast);
    auto *oneCall = b.CreateCall(bounded, args);
    b.CreateRet(oneCall);
    InlineFunctionInfo boundedInfo;
    if (!InlineFunction(*oneCall, boundedInfo).isSuccess()) {
      wrapper->eraseFromParent();
      bounded->eraseFromParent();
      continue;
    }
    bounded->eraseFromParent();
    b.SetInsertPoint(wide);
    auto *tail = b.CreateCall(original, args);
    tail->setCallingConv(original->getCallingConv());
    tail->setAttributes(original->getAttributes().removeFnAttributes(ctx));
    tail->setTailCallKind(CallInst::TCK_MustTail);
    b.CreateRet(tail);
    simplify(*wrapper, true);
    if (verifyFunction(*wrapper) || nativeStackBudgetError(*wrapper, module.getDataLayout())) {
      wrapper->eraseFromParent();
      continue;
    }
    auto name = original->getName().str();
    original->setName(name + ".full-integer");
    original->setLinkage(GlobalValue::InternalLinkage);
    original->addFnAttr(Attribute::NoInline);
    wrapper->setName(name);
    wrapper->addFnAttr("kalandra.ordinary.integer-cases");
    if (stable)
      wrapper->addFnAttr("kalandra.ordinary.stable-countdown");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
