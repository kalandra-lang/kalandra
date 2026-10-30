#pragma once
#include "../runtime/Runtime.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>
#include <optional>

namespace kalandra {
// Absence of a token is an execution contract, not a claim about whether the
// source spawns tasks. Only a closed, allocation-free source graph can bypass
// admission: no foreign callbacks, task operations or recoverable stack growth.
// Unlike bounded cancellable entries, an unbound loop needs no latency budget.
inline bool addUnboundEntries(llvm::Module &module,
                              const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<CallInst *, 16> entries;
  for (auto &fn : module)
    if (!fn.isDeclaration() && !fn.hasFnAttribute("kalandra.task.frame"))
      for (auto &block : fn)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && call->getCalledFunction() &&
              call->getCalledFunction()->getName() == "kalandra_source_run_v4")
            entries.push_back(call);
  bool changed = false;
  for (auto *entry : entries) {
    auto *original = dyn_cast<llvm::Function>(entry->getArgOperand(1));
    if (!original || !original->hasFnAttribute("kalandra.boundary"))
      continue;
    std::int64_t packetOffset = 0;
    auto *packet = GetPointerBaseWithConstantOffset(entry->getArgOperand(2), packetOffset,
                                                    module.getDataLayout());
    if (!isa<AllocaInst>(packet))
      continue;
    DominatorTree dom(*entry->getFunction());
    std::optional<std::int64_t> contextOffset;
    SmallVector<std::pair<std::int64_t, std::int64_t>, 16> initialized;
    for (auto &block : *entry->getFunction())
      for (auto &instruction : block)
        if (auto *store = dyn_cast<StoreInst>(&instruction);
            store && store->isSimple() && store->getValueOperand() == entry->getArgOperand(0) &&
            dom.dominates(store, entry)) {
          std::int64_t offset = 0;
          if (GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                               module.getDataLayout()) == packet)
            contextOffset = offset - packetOffset;
        }
    if (!contextOffset)
      continue;
    for (auto &block : *entry->getFunction())
      for (auto &instruction : block)
        if (auto *store = dyn_cast<StoreInst>(&instruction);
            store && store->isSimple() && dom.dominates(store, entry)) {
          std::int64_t offset = 0;
          if (GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                               module.getDataLayout()) == packet)
            initialized.emplace_back(
                offset - packetOffset,
                offset - packetOffset +
                    module.getDataLayout().getTypeStoreSize(store->getValueOperand()->getType()));
        }
    ValueToValueMapTy mapping;
    auto *body = CloneFunction(original, mapping);
    body->setName(original->getName() + ".unbound");
    body->setSection("");
    body->removeFnAttr("kalandra.boundary");
    body->removeFnAttr("kalandra.native.pure-frame");
    bool viable = true;
    for (unsigned round = 0; round < 12; ++round) {
      SmallVector<LoadInst *, 8> modes;
      for (auto &block : *body)
        for (auto &instruction : block)
          if (auto *load = dyn_cast<LoadInst>(&instruction);
              load && load->isSimple() && load->getType()->isIntegerTy(8)) {
            std::int64_t offset = 0, position = 0;
            auto *base = GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                                          module.getDataLayout());
            auto *ctx = dyn_cast<LoadInst>(base);
            if (offset == std::int64_t(runtime::InvocationABI::pollMode) && ctx &&
                ctx->isSimple() && ctx->getType()->isPointerTy() &&
                GetPointerBaseWithConstantOffset(ctx->getPointerOperand(), position,
                                                 module.getDataLayout()) == body->getArg(0) &&
                position == *contextOffset)
              modes.push_back(load);
          }
      for (auto *load : modes) {
        load->replaceAllUsesWith(ConstantInt::get(load->getType(), 0));
        load->eraseFromParent();
      }
      simplify(*body, true);
      SmallVector<CallInst *, 8> calls;
      unsigned size = 0;
      for (auto &block : *body)
        for (auto &instruction : block) {
          ++size;
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && call->getCalledFunction() && !call->getCalledFunction()->isDeclaration() &&
              call->getCalledFunction()->getName().contains(".body.clear"))
            calls.push_back(call);
        }
      if (size > 2048) {
        viable = false;
        break;
      }
      if (calls.empty())
        break;
      for (auto *call : calls) {
        for (auto &block : *call->getCalledFunction())
          size += block.size();
        InlineFunctionInfo info;
        if (size > 2048 || !InlineFunction(*call, info).isSuccess()) {
          viable = false;
          break;
        }
      }
      if (!viable)
        break;
    }
    // Once inlined, this is the outermost source callback: a private abort
    // exits this callback rather than unwinding another source frame.
    SmallVector<CallInst *, 8> aborts;
    for (auto &block : *body)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "kalandra_source_abort_v2" &&
            isa<UnreachableInst>(block.getTerminator()))
          aborts.push_back(call);
    for (auto *call : aborts) {
      auto *block = call->getParent();
      auto cursor = call->getIterator();
      while (cursor != block->end())
        cursor = cursor->eraseFromParent();
      IRBuilder<> b(block);
      b.CreateRet(b.getFalse());
    }
    simplify(*body, true);
    // The callback must finish faults locally. Throwing/allocating/calling work
    // still needs the portable/native boundary even with no cancellation token.
    for (auto &block : *body)
      for (auto &instruction : block) {
        if (isa<AllocaInst>(instruction) || isa<InvokeInst>(instruction) ||
            isa<IndirectBrInst>(instruction) || instruction.isAtomic())
          viable = false;
        if (auto *load = dyn_cast<LoadInst>(&instruction)) {
          std::int64_t offset = 0;
          bool known = false;
          if (load->isSimple() &&
              GetPointerBaseWithConstantOffset(load->getPointerOperand(), offset,
                                               module.getDataLayout()) == body->getArg(0)) {
            auto end =
                offset + std::int64_t(module.getDataLayout().getTypeStoreSize(load->getType()));
            for (auto [first, last] : initialized)
              known |= offset >= first && end <= last;
          }
          if (!known)
            viable = false;
        }
        if (auto *store = dyn_cast<StoreInst>(&instruction)) {
          std::int64_t offset = 0;
          if (!store->isSimple() ||
              GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                               module.getDataLayout()) != body->getArg(0))
            viable = false;
          // Packet context cannot be rebound before another folded mode load.
          auto bytes = std::int64_t(
              module.getDataLayout().getTypeStoreSize(store->getValueOperand()->getType()));
          if (offset < *contextOffset + 8 && *contextOffset < offset + bytes)
            viable = false;
        }
        if ((instruction.getOpcode() == Instruction::UDiv ||
             instruction.getOpcode() == Instruction::SDiv ||
             instruction.getOpcode() == Instruction::URem ||
             instruction.getOpcode() == Instruction::SRem) &&
            (!instruction.getType()->isIntegerTy() ||
             instruction.getType()->getIntegerBitWidth() > 64))
          viable = false;
        if (auto *call = dyn_cast<CallBase>(&instruction)) {
          auto *target = call->getCalledFunction();
          if (!target || !isa<CallInst>(call)) {
            viable = false;
            continue;
          }
          if (target->getName() == "kalandra_fault_v1") {
            auto *next = call->getNextNode();
            auto *ret = dyn_cast<ReturnInst>(next);
            Value *value = ret ? ret->getReturnValue() : nullptr;
            if (auto *branch = dyn_cast<UncondBrInst>(next)) {
              auto *exit = branch->getSuccessor(0);
              ret = dyn_cast<ReturnInst>(exit->getTerminator());
              bool onlyReturn = true;
              for (auto &i : *exit)
                onlyReturn &= isa<PHINode>(i) || isa<ReturnInst>(i);
              if (ret && onlyReturn) {
                value = ret->getReturnValue();
                if (auto *phi = dyn_cast<PHINode>(value))
                  value = phi->getIncomingValueForBlock(&block);
              }
            }
            auto *constant = dyn_cast_or_null<ConstantInt>(value);
            if (!constant || !constant->isZero())
              viable = false;
          } else if (target->isIntrinsic()) {
            switch (target->getIntrinsicID()) {
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
            case Intrinsic::assume:
            case Intrinsic::expect:
            case Intrinsic::expect_with_probability:
            case Intrinsic::smax:
            case Intrinsic::smin:
            case Intrinsic::umax:
            case Intrinsic::umin:
            case Intrinsic::abs:
            case Intrinsic::bswap:
            case Intrinsic::bitreverse:
            case Intrinsic::ctpop:
            case Intrinsic::ctlz:
            case Intrinsic::cttz:
            case Intrinsic::sadd_with_overflow:
            case Intrinsic::uadd_with_overflow:
            case Intrinsic::ssub_with_overflow:
            case Intrinsic::usub_with_overflow:
            case Intrinsic::smul_with_overflow:
            case Intrinsic::umul_with_overflow:
              break;
            default:
              viable = false;
            }
          } else
            viable = false;
        }
      }
    if (!viable) {
      body->eraseFromParent();
      continue;
    }
    body->addFnAttr(Attribute::NoUnwind);
    body->addFnAttr("kalandra.unbound.entry");
    auto *before = entry->getParent();
    auto *slow = before->splitBasicBlock(entry->getIterator(), "entry.managed");
    auto *after = slow->splitBasicBlock(std::next(entry->getIterator()), "entry.unbound.resume");
    auto *run = BasicBlock::Create(module.getContext(), "entry.unbound", before->getParent(), slow);
    before->getTerminator()->eraseFromParent();
    IRBuilder<> b(before);
    auto *mode =
        b.CreateLoad(b.getInt8Ty(), b.CreateConstGEP1_64(b.getInt8Ty(), entry->getArgOperand(0),
                                                         runtime::InvocationABI::pollMode));
    b.CreateCondBr(b.CreateICmpEQ(mode, b.getInt8(0)), run, slow);
    b.SetInsertPoint(run);
    auto *complete = b.CreateCall(body, {entry->getArgOperand(2)});
    b.CreateBr(after);
    b.SetInsertPoint(&*after->begin());
    auto *joined = b.CreatePHI(b.getInt1Ty(), 2);
    entry->replaceAllUsesWith(joined);
    joined->addIncoming(entry, slow);
    joined->addIncoming(complete, run);
    InlineFunctionInfo info;
    if (!InlineFunction(*complete, info).isSuccess())
      llvm_unreachable("unbound callback must inline");
    if (body->use_empty())
      body->eraseFromParent();
    // Mark before simplification: it may merge/delete the originating block.
    before->getParent()->addFnAttr("kalandra.unbound.host-entry");
    simplify(*before->getParent(), true);
    changed = true;
  }
  return changed;
}
} // namespace kalandra
