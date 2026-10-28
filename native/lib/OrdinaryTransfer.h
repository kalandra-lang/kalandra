#pragma once
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IntrinsicInst.h"
namespace kalandra {
inline bool ordinaryTerminalTransfer(llvm::CallInst &call) {
  using namespace llvm;
  if (!call.hasOneUse())
    return false;
  Value *value = &call;
  Instruction *next = call.getNextNode();
  SmallPtrSet<BasicBlock *, 8> visited;
  auto *from = call.getParent();
  while (next) {
    if (auto *intrinsic = dyn_cast<IntrinsicInst>(next);
        intrinsic && intrinsic->getIntrinsicID() == Intrinsic::lifetime_end) {
      next = next->getNextNode();
      continue;
    }
    if (auto *ret = dyn_cast<ReturnInst>(next))
      return ret->getReturnValue() == value;
    auto *branch = dyn_cast<UncondBrInst>(next);
    if (!branch)
      return false;
    auto *block = branch->getSuccessor(0);
    if (!visited.insert(block).second)
      return false;
    for (auto &phi : block->phis())
      if (phi.getIncomingValueForBlock(from) == value) {
        if (!phi.hasOneUse())
          return false;
        value = &phi;
        break;
      }
    from = block;
    next = &*block->getFirstNonPHIOrDbg();
  }
  return false;
}

} // namespace kalandra
