#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <cmath>
#include <functional>

namespace kalandra {
// Prove only the small IEEE subset needed here. No reassociation, contraction,
// infinity exclusion, or assumptions about caller-provided floating values.
inline bool ordinaryFinite(llvm::Value *value, unsigned depth = 0) {
  using namespace llvm;
  if (depth > 32)
    return false;
  if (auto *c = dyn_cast<ConstantFP>(value))
    return c->getValueAPF().isFinite();
  auto *i = dyn_cast<Instruction>(value);
  if (!i)
    return false;
  if ((isa<SIToFPInst>(i) || isa<UIToFPInst>(i)) &&
      (i->getType()->isFloatTy() || i->getType()->isDoubleTy()))
    return i->getOperand(0)->getType()->getIntegerBitWidth() <= 64;
  if (i->getOpcode() == Instruction::FNeg)
    return ordinaryFinite(i->getOperand(0), depth + 1);
  if (i->getOpcode() == Instruction::FMul)
    for (unsigned k = 0; k < 2; ++k)
      if (auto *c = dyn_cast<ConstantFP>(i->getOperand(k));
          c && c->getValueAPF().isFinite() && std::abs(c->getValueAPF().convertToDouble()) <= 1.0)
        return ordinaryFinite(i->getOperand(1 - k), depth + 1);
  return false;
}
inline bool ordinaryNoNaN(llvm::Value *value, llvm::SmallPtrSetImpl<llvm::Value *> &seen,
                          unsigned &budget) {
  using namespace llvm;
  if (!budget)
    return false;
  --budget;
  if (!seen.insert(value).second)
    return true;
  if (auto *c = dyn_cast<ConstantFP>(value))
    return !c->getValueAPF().isNaN();
  if (ordinaryFinite(value))
    return true;
  auto *i = dyn_cast<Instruction>(value);
  if (!i)
    return false;
  auto visit = [&](Value *v) { return ordinaryNoNaN(v, seen, budget); };
  if (auto *phi = dyn_cast<PHINode>(i)) {
    for (auto &v : phi->incoming_values())
      if (!visit(v))
        return false;
    return true;
  }
  if (auto *select = dyn_cast<SelectInst>(i))
    return visit(select->getTrueValue()) && visit(select->getFalseValue());
  if (isa<FPExtInst>(i) || isa<FPTruncInst>(i) || i->getOpcode() == Instruction::FNeg)
    return visit(i->getOperand(0));
  if (i->getOpcode() == Instruction::FMul) {
    for (unsigned k = 0; k < 2; ++k)
      if (auto *c = dyn_cast<ConstantFP>(i->getOperand(k));
          c && c->getValueAPF().isFinite() && !c->getValueAPF().isZero())
        return visit(i->getOperand(1 - k));
    return ordinaryFinite(i->getOperand(0)) && ordinaryFinite(i->getOperand(1));
  }
  if (i->getOpcode() == Instruction::FDiv)
    if (auto *c = dyn_cast<ConstantFP>(i->getOperand(1));
        c && c->getValueAPF().isFinite() && !c->getValueAPF().isZero())
      return visit(i->getOperand(0));
  if (i->getOpcode() == Instruction::FAdd || i->getOpcode() == Instruction::FSub) {
    if (ordinaryFinite(i->getOperand(0)))
      return visit(i->getOperand(1));
    if (ordinaryFinite(i->getOperand(1)))
      return visit(i->getOperand(0));
  }
  return false;
}
inline bool refineOrdinaryNaNTests(llvm::Function &fn) {
  using namespace llvm;
  SmallVector<FCmpInst *, 8> tests;
  for (auto &b : fn)
    for (auto &i : b)
      if (auto *cmp = dyn_cast<FCmpInst>(&i); cmp && (cmp->getPredicate() == CmpInst::FCMP_UNO ||
                                                      cmp->getPredicate() == CmpInst::FCMP_ORD))
        tests.push_back(cmp);
  bool changed = false;
  for (auto *test : tests) {
    SmallPtrSet<Value *, 32> seen;
    unsigned budget = 256;
    if (!ordinaryNoNaN(test->getOperand(0), seen, budget))
      continue;
    seen.clear();
    budget = 256;
    if (!ordinaryNoNaN(test->getOperand(1), seen, budget))
      continue;
    test->replaceAllUsesWith(
        ConstantInt::get(test->getType(), test->getPredicate() == CmpInst::FCMP_ORD));
    test->eraseFromParent();
    changed = true;
  }
  return changed;
}
// A two-block countdown's MIN case is unreachable for a nonnegative initial
// count: zero exits, and the sole backedge subtracts exactly one. Version the
// entire ordinary entry behind that guard; negative inputs keep the old worker.
inline bool optimizeOrdinaryLoops(llvm::Module &module,
                                  const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        !fn.hasFnAttribute("kalandra.ordinary.local-buffers"))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *fn : roots) {
    if (refineOrdinaryNaNTests(*fn)) {
      simplify(*fn, true);
      changed = true;
    }
    unsigned size = 0;
    for (auto &b : *fn)
      size += b.size();
    if (size > 256)
      continue;
    DominatorTree dt(*fn);
    LoopInfo loops(dt);
    SwitchInst *chosen = nullptr;
    Argument *argument = nullptr;
    for (auto &block : *fn) {
      auto *sw = dyn_cast<SwitchInst>(block.getTerminator());
      if (!sw || sw->getNumCases() != 2)
        continue;
      auto *phi = dyn_cast<PHINode>(sw->getCondition());
      auto *loop = loops.getLoopFor(&block);
      if (!phi || phi->getNumIncomingValues() != 2 || !loop || loop->getHeader() != &block ||
          loop->getNumBlocks() != 2)
        continue;
      auto *latch = loop->getLoopLatch();
      if (!latch || sw->getDefaultDest() != latch)
        continue;
      auto *branch = dyn_cast<UncondBrInst>(latch->getTerminator());
      if (!branch || branch->getSuccessor(0) != &block)
        continue;
      bool zero = false, minimum = false, outside = true;
      for (auto c : sw->cases()) {
        zero |= c.getCaseValue()->isZero();
        minimum |= c.getCaseValue()->getValue().isMinSignedValue();
        outside &= !loop->contains(c.getCaseSuccessor());
      }
      if (!zero || !minimum || !outside)
        continue;
      auto initial = loop->contains(phi->getIncomingBlock(0)) ? 1U : 0U;
      auto *arg = dyn_cast<Argument>(phi->getIncomingValue(initial));
      auto *step = dyn_cast<BinaryOperator>(phi->getIncomingValue(1 - initial));
      if (!arg || arg->getParent() != fn || arg->getType()->getIntegerBitWidth() < 2 || !step ||
          step->getParent() != latch)
        continue;
      auto *amount = dyn_cast<ConstantInt>(step->getOperand(1));
      if (step->getOperand(0) != phi || !amount ||
          !((step->getOpcode() == Instruction::Add && amount->isMinusOne()) ||
            (step->getOpcode() == Instruction::Sub && amount->equalsInt(1))))
        continue;
      chosen = sw;
      argument = arg;
      break;
    }
    if (!chosen)
      continue;
    auto argNumber = argument->getArgNo();
    ValueToValueMapTy fastMap;
    auto *fast = CloneFunction(fn, fastMap);
    fast->setLinkage(GlobalValue::InternalLinkage);
    fast->setName(fn->getName() + ".countdown");
    fast->removeFnAttr("kalandra.ordinary.entry");
    auto *sw = cast<SwitchInst>(fastMap[chosen]);
    for (auto it = sw->case_begin(); it != sw->case_end(); ++it)
      if (it->getCaseValue()->getValue().isMinSignedValue()) {
        auto *destination = it->getCaseSuccessor();
        sw->removeCase(it);
        destination->removePredecessor(sw->getParent());
        break;
      }
    simplify(*fast, true);
    ValueToValueMapTy slowMap;
    auto *slow = CloneFunction(fn, slowMap);
    slow->setLinkage(GlobalValue::InternalLinkage);
    slow->setName(fn->getName() + ".signed-countdown");
    slow->removeFnAttr("kalandra.ordinary.entry");
    slow->addFnAttr(Attribute::NoInline);
    fn->deleteBody();
    auto &ctx = module.getContext();
    IRBuilder<> b(BasicBlock::Create(ctx, "ordinary.count", fn));
    auto *positive = BasicBlock::Create(ctx, "ordinary.nonnegative", fn),
         *negative = BasicBlock::Create(ctx, "ordinary.negative", fn);
    b.CreateCondBr(b.CreateICmpSGE(fn->getArg(argNumber), ConstantInt::get(argument->getType(), 0)),
                   positive, negative);
    SmallVector<Value *, 8> args;
    for (auto &arg : fn->args())
      args.push_back(&arg);
    b.SetInsertPoint(positive);
    auto *call = b.CreateCall(fast, args);
    call->setAttributes(fast->getAttributes().removeFnAttributes(ctx));
    call->setCallingConv(fast->getCallingConv());
    b.CreateRet(call);
    InlineFunctionInfo info;
    if (!InlineFunction(*call, info).isSuccess())
      llvm_unreachable("ordinary countdown must inline");
    if (fast->use_empty())
      fast->eraseFromParent();
    b.SetInsertPoint(negative);
    auto *tail = b.CreateCall(slow, args);
    tail->setAttributes(slow->getAttributes().removeFnAttributes(ctx));
    tail->setCallingConv(slow->getCallingConv());
    tail->setTailCallKind(CallInst::TCK_MustTail);
    b.CreateRet(tail);
    fn->addFnAttr("kalandra.ordinary.countdown");
    simplify(*fn, true);
    changed = true;
  }
  return changed;
}
} // namespace kalandra
