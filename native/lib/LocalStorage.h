#pragma once
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <optional>

namespace kalandra {
// After native inlining, some closure environments are only local memory.
// Keep escaped/transported environments in the invocation ledger. In particular,
// do not infer non-escape from a closure's source type or from reference counts.
inline bool promoteLocalEnvironments(llvm::Module &module,
                                     std::optional<std::uint64_t> pollModeOffset = {}) {
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
    // Clear workers are entered only in unbound or armed native-interrupt
    // mode. Once this allocation disappears, its helper-status diamond has
    // neither a possible allocation fault nor a cooperative poll obligation.
    // Watched/cooperative workers retain the diamond. Match the exact ABI
    // field and isolated slow-poll block; uncertain control flow stays checked.
    if (pollModeOffset && fn.getName().ends_with(".body.clear")) {
      auto *dispatch = dyn_cast<SwitchInst>(call->getParent()->getTerminator());
      auto *mode = dispatch ? dyn_cast<LoadInst>(dispatch->getCondition()) : nullptr;
      std::int64_t offset = 0;
      auto *base = mode ? GetPointerBaseWithConstantOffset(mode->getPointerOperand(), offset,
                                                          module.getDataLayout()) : nullptr;
      if (dispatch && (dispatch->getNumCases() == 2 || dispatch->getNumCases() == 3) && mode && mode->isSimple() &&
          base == call->getArgOperand(0) && offset == std::int64_t(*pollModeOffset)) {
        auto clear = dispatch->findCaseValue(ConstantInt::get(cast<IntegerType>(mode->getType()), 0));
        auto token = dispatch->findCaseValue(ConstantInt::get(cast<IntegerType>(mode->getType()), 1));
        auto armed = dispatch->findCaseValue(ConstantInt::get(cast<IntegerType>(mode->getType()), 3));
        auto *observe = dispatch->getDefaultDest();
        bool isolated = clear != dispatch->case_default() && token != dispatch->case_default() &&
            clear->getCaseSuccessor() != observe && token->getCaseSuccessor() != observe &&
            clear->getCaseSuccessor() != token->getCaseSuccessor() &&
            observe->getSinglePredecessor() == call->getParent();
        if (dispatch->getNumCases() == 3)
          isolated &= armed != dispatch->case_default() && token != dispatch->case_default() &&
              armed->getCaseSuccessor() == token->getCaseSuccessor();
        bool poll = false;
        for (auto it = std::next(call->getIterator()); it != call->getParent()->end(); ++it)
          if (&*it != dispatch && it->mayHaveSideEffects()) isolated = false;
        for (auto &instruction : *observe)
          if (auto *slow = dyn_cast<CallInst>(&instruction)) {
            const bool expected = slow->getCalledFunction() &&
                slow->getCalledFunction()->getName() == "kalandra_poll_slow_v1" &&
                slow->arg_size() == 1 && slow->getArgOperand(0) == call->getArgOperand(0);
            isolated &= expected && !poll;
            poll |= expected;
          } else if (instruction.mayHaveSideEffects()) isolated = false;
        if (isolated && poll) {
          auto *parent = call->getParent();
          auto *success = clear->getCaseSuccessor();
          observe->removePredecessor(parent, true);
          token->getCaseSuccessor()->removePredecessor(parent, true);
          if (dispatch->getNumCases() == 3)
            token->getCaseSuccessor()->removePredecessor(parent, true);
          UncondBrInst::Create(success, dispatch->getIterator());
          dispatch->eraseFromParent();
        }
      }
    }
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
