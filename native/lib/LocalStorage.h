#pragma once
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// After native inlining, some closure environments are only local memory.
// Keep escaped/transported environments in the invocation ledger. In particular,
// do not infer non-escape from a closure's source type or from reference counts.
inline bool promoteLocalEnvironments(llvm::Module &module) {
  using namespace llvm;
  SmallVector<CallInst *, 16> candidates;
  for (auto &fn : module)
    if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame"))
      for (auto &block : fn)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && call->getCalledFunction() &&
              call->getCalledFunction()->getName() == "kalandra_environment_alloc_v1")
            candidates.push_back(call);
  bool changed = false;
  for (auto *call : candidates) {
    auto *bytes = dyn_cast<ConstantInt>(call->getArgOperand(1));
    if (!bytes || bytes->isZero() || bytes->getValue().ugt(256)) continue;
    // An entry alloca can be reused only if this allocation executes at most
    // once per call. Check all CFG cycles, including irreducible ones.
    SmallPtrSet<BasicBlock *, 32> visited;
    SmallVector<BasicBlock *, 16> blocks;
    for (auto *next : successors(call->getParent())) blocks.push_back(next);
    bool safe = true;
    while (!blocks.empty() && safe) {
      auto *block = blocks.pop_back_val();
      if (block == call->getParent()) { safe = false; break; }
      if (!visited.insert(block).second) continue;
      for (auto *next : successors(block)) blocks.push_back(next);
    }
    SmallPtrSet<Value *, 16> seen;
    SmallVector<Value *, 16> pointers{call};
    while (!pointers.empty() && safe) {
      auto *pointer = pointers.pop_back_val();
      if (!seen.insert(pointer).second) continue;
      for (auto *use : pointer->users()) {
        if (auto *load = dyn_cast<LoadInst>(use)) {
          safe &= load->getPointerOperand() == pointer && load->isSimple();
        } else if (auto *store = dyn_cast<StoreInst>(use)) {
          safe &= store->getPointerOperand() == pointer &&
                  store->getValueOperand() != pointer && store->isSimple();
        } else if (isa<GetElementPtrInst>(use) || isa<BitCastInst>(use)) {
          pointers.push_back(cast<Value>(use));
        } else {
          // Returns, aggregates, PHIs, pointer identity, foreign calls and even
          // other known native calls stay owned. No tail transfer may borrow
          // the new frame-local address.
          safe = false;
        }
      }
    }
    if (!safe) continue;
    auto &fn = *call->getFunction();
    IRBuilder<> entry(&*fn.getEntryBlock().getFirstInsertionPt());
    auto *storage = entry.CreateAlloca(ArrayType::get(entry.getInt8Ty(), bytes->getZExtValue()),
                                      nullptr, "local.environment");
    storage->setAlignment(Align(16));
    call->replaceAllUsesWith(storage);
    call->eraseFromParent();
    // No call receives this address. Existing guaranteed tail transfers remain
    // safe: every use of the local environment precedes the transfer.
    changed = true;
  }
  return changed;
}
} // namespace kalandra
