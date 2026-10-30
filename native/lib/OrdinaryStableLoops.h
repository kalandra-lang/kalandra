#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <optional>

namespace kalandra {
// A positive countdown terminates. If the remaining integer state is unchanged
// and the pure body cannot observe the counter, every remaining iteration has
// the same result. Apply only to expensive, memory-free ordinary computations;
// managed progress/cancellation and observable effects cannot enter this proof.
inline bool accelerateOrdinaryStableLoop(llvm::Function &fn) {
  using namespace llvm;
  if (!fn.hasFnAttribute("kalandra.ordinary.direct") ||
      fn.hasFnAttribute("kalandra.ordinary.stable-countdown"))
    return false;
  DominatorTree dom(fn);
  LoopInfo loops(dom);
  for (auto *loop : loops.getLoopsInPreorder()) {
    auto *latch = loop->getLoopLatch();
    if (!latch || loop->getExitingBlock() != latch)
      continue;
    auto *branch = dyn_cast<CondBrInst>(latch->getTerminator());
    if (!branch)
      continue;
    auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
    if (!cmp || !cmp->hasOneUse() ||
        (cmp->getPredicate() != ICmpInst::ICMP_EQ && cmp->getPredicate() != ICmpInst::ICMP_NE))
      continue;
    auto *step = dyn_cast<BinaryOperator>(cmp->getOperand(0));
    auto *zero = dyn_cast<ConstantInt>(cmp->getOperand(1));
    if (!step || !zero || !zero->isZero() || step->getOpcode() != Instruction::Add)
      continue;
    auto *counter = dyn_cast<PHINode>(step->getOperand(0));
    auto *minusOne = dyn_cast<ConstantInt>(step->getOperand(1));
    if (!counter || counter->getParent() != loop->getHeader() ||
        counter->getNumIncomingValues() != 2 || !minusOne || !minusOne->isMinusOne())
      continue;
    int back = counter->getBasicBlockIndex(latch);
    if (back < 0 || counter->getIncomingValue(back) != step ||
        loop->contains(counter->getIncomingBlock(1 - back)))
      continue;
    auto *initial = dyn_cast<Argument>(counter->getIncomingValue(1 - back));
    auto range = initial ? initial->getRange() : std::nullopt;
    if (!range || range->isEmptySet() || range->getUnsignedMin().isZero() ||
        range->getUnsignedMax().isNegative())
      continue;
    const unsigned exitIndex = cmp->getPredicate() == ICmpInst::ICMP_EQ ? 0 : 1;
    auto *exit = branch->getSuccessor(exitIndex);
    if (loop->contains(exit) || branch->getSuccessor(1 - exitIndex) != loop->getHeader())
      continue;
    bool valid = true, expensive = false;
    for (auto *user : counter->users())
      valid &= user == step;
    for (auto *user : step->users())
      valid &= user == counter || user == cmp;
    for (auto *block : loop->blocks())
      for (auto &i : *block) {
        expensive |= i.getOpcode() == Instruction::URem || i.getOpcode() == Instruction::SRem ||
                     i.getOpcode() == Instruction::UDiv || i.getOpcode() == Instruction::SDiv;
        if (i.mayReadOrWriteMemory() || (!i.isTerminator() && i.mayHaveSideEffects()) ||
            isa<CallBase>(i) || isa<AllocaInst>(i) || isa<FreezeInst>(i) ||
            i.getType()->isPointerTy())
          valid = false;
        for (auto &operand : i.operands())
          if (isa<UndefValue>(operand) || isa<PoisonValue>(operand))
            valid = false;
      }
    SmallVector<PHINode *, 4> states;
    for (auto &phi : loop->getHeader()->phis()) {
      if (&phi == counter)
        continue;
      if (!phi.getType()->isIntegerTy() || phi.getNumIncomingValues() != 2 ||
          phi.getBasicBlockIndex(latch) < 0)
        valid = false;
      states.push_back(&phi);
    }
    if (!valid || !expensive || states.empty())
      continue;
    auto *check = BasicBlock::Create(fn.getContext(), "ordinary.stable-state", &fn);
    IRBuilder<> b(check);
    Value *same = b.getTrue();
    for (auto *phi : states) {
      auto *next = phi->getIncomingValueForBlock(latch);
      same = b.CreateAnd(same, b.CreateICmpEQ(b.CreateFreeze(phi), b.CreateFreeze(next)));
    }
    auto *next = b.CreateCondBr(same, exit, loop->getHeader());
    next->setMetadata(LLVMContext::MD_loop, branch->getMetadata(LLVMContext::MD_loop));
    branch->setMetadata(LLVMContext::MD_loop, nullptr);
    branch->setSuccessor(1 - exitIndex, check);
    for (auto &phi : loop->getHeader()->phis())
      phi.setIncomingBlock(phi.getBasicBlockIndex(latch), check);
    for (auto &phi : exit->phis())
      phi.addIncoming(phi.getIncomingValueForBlock(latch), check);
    fn.addFnAttr("kalandra.ordinary.stable-countdown");
    return true;
  }
  return false;
}
} // namespace kalandra
