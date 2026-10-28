#pragma once
#include "NativeStack.h"
#include "OrdinaryTransfer.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// Only compiler-generated scalar worker/replay transfers are admitted. Their
// private pools do not escape. Keep every transfer a musttail: concatenating
// the workers' bounded frames would invalidate the original stack admission.

inline bool
addOrdinaryBufferDirectEntries(llvm::Module &module,
                               const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        fn.hasFnAttribute("kalandra.ordinary.local-buffers") &&
        fn.getName().ends_with("_ordinary_v1"))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *root : roots) {
    if (root->arg_empty() || !root->getArg(root->arg_size() - 1)->getType()->isPointerTy())
      continue;
    auto name = root->getName().str();
    name.resize(name.size() - std::string("ordinary_v1").size());
    if (module.getFunction(name + "ordinary_direct_v1"))
      continue;
    SmallVector<llvm::Function *, 8> family{root};
    SmallPtrSet<llvm::Function *, 8> seen{root};
    bool valid = true;
    for (unsigned n = 0; valid && n < family.size(); ++n) {
      auto *fn = family[n];
      const bool heap = fn->hasFnAttribute("kalandra.ordinary.fallback");
      CallInst *begin = nullptr, *end = nullptr;
      for (auto &block : *fn)
        for (auto &i : block) {
          auto *call = dyn_cast<CallInst>(&i);
          if (!call)
            continue;
          auto *callee = call->getCalledFunction();
          if (callee && callee->isIntrinsic())
            continue;
          if (call->isInlineAsm() && call->getMetadata("kalandra.ordinary.fixed-convert") &&
              !cast<InlineAsm>(call->getCalledOperand())->hasSideEffects())
            continue;
          if (heap) {
            // Generated replay owns a fresh Invocation. It must settle before
            // any return; never turn its checked computation into a direct call.
            if (callee && callee->getName() == "kalandra_ordinary_begin_v1") {
              valid = valid && !begin;
              begin = call;
            }
            if (callee && callee->getName() == "kalandra_ordinary_end_v1") {
              valid = valid && !end;
              end = call;
            }
            continue;
          }
          if (callee && callee->getName() == "kalandra_ordinary_local_enabled_v1" &&
              call->arg_empty() && call->getType()->isIntegerTy(64))
            continue;
          if (!callee || callee->isDeclaration() || !callee->hasInternalLinkage() ||
              !(callee->hasFnAttribute("kalandra.ordinary.fallback") ||
                callee->getName().contains("ordinary_v1.")) ||
              callee->getFunctionType() != root->getFunctionType() ||
              call->getArgOperand(call->arg_size() - 1) != fn->getArg(fn->arg_size() - 1) ||
              !ordinaryTerminalTransfer(*call)) {
            valid = false;
            break;
          }
          if (seen.insert(callee).second)
            family.push_back(callee);
        }
      if (heap) {
        DominatorTree dom(*fn);
        valid = valid && begin && end && dom.dominates(begin, end) &&
                begin->getArgOperand(0) == end->getArgOperand(0);
        for (auto &block : *fn)
          if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
            valid = valid && end && dom.dominates(end, ret);
      }
    }
    if (!valid)
      continue;
    SmallVector<Type *, 8> params;
    SmallVector<AttributeSet, 8> attrs;
    for (unsigned n = 0; n + 1 < root->arg_size(); ++n) {
      auto *type = root->getArg(n)->getType();
      // Scalar arguments cannot borrow storage from a discarded pool frame.
      valid = valid && (type->isIntegerTy() || type->isFloatingPointTy());
      params.push_back(type);
      attrs.push_back(root->getAttributes().getParamAttrs(n));
    }
    if (!valid)
      continue;
    auto &ctx = module.getContext();
    auto *type = FunctionType::get(root->getReturnType(), params, false);
    DenseMap<llvm::Function *, llvm::Function *> copies;
    for (auto *fn : family) {
      auto *copy = llvm::Function::Create(
          type, fn == root ? root->getLinkage() : GlobalValue::InternalLinkage,
          fn == root ? name + "ordinary_direct_v1" : fn->getName().str() + ".direct", module);
      copy->setCallingConv(fn->getCallingConv());
      copy->setAttributes(AttributeList::get(ctx, fn->getAttributes().getFnAttrs(),
                                             fn->getAttributes().getRetAttrs(), attrs));
      copies[fn] = copy;
    }
    for (auto *fn : family) {
      auto *copy = copies[fn];
      IRBuilder<> b(BasicBlock::Create(ctx, "ordinary.direct", copy));
      auto *fault =
          b.CreateAlloca(ArrayType::get(b.getInt64Ty(), 3), nullptr, "ordinary.cold-fault");
      b.CreateMemSet(fault, b.getInt8(0), 24, Align(8));
      ValueToValueMapTy map;
      for (unsigned n = 0; n < copy->arg_size(); ++n)
        map[fn->getArg(n)] = copy->getArg(n);
      map[fn->getArg(fn->arg_size() - 1)] = fault;
      SmallVector<ReturnInst *, 8> returns;
      CloneFunctionInto(copy, fn, map, CloneFunctionChangeType::LocalChangesOnly, returns);
      // LLVM 23's AArch64 TypePromotion follows narrow argument users into
      // zeroext musttail calls, inserting zext/trunc after the call and then
      // rejecting it during ISel. A fake-use keeps that promotion tree out
      // without changing the ABI, emitting code, or disabling optimization.
      if (module.getTargetTriple().isAArch64() && copy->getReturnType()->isIntegerTy() &&
          copy->getReturnType()->getIntegerBitWidth() < 32 &&
          fn->hasRetAttribute(Attribute::ZExt)) {
        SmallVector<Value *, 8> inputs;
        for (auto &arg : copy->args())
          inputs.push_back(&arg);
        if (!inputs.empty())
          b.CreateCall(Intrinsic::getOrInsertDeclaration(&module, Intrinsic::fake_use), inputs);
      }
      b.CreateBr(cast<BasicBlock>(map[&fn->getEntryBlock()]));
      copy->setAttributes(AttributeList::get(ctx, fn->getAttributes().getFnAttrs(),
                                             fn->getAttributes().getRetAttrs(), attrs));
      copy->removeFnAttr("kalandra.ordinary.entry");
      copy->addFnAttr("kalandra.ordinary.direct");
      copy->addFnAttr(Attribute::NoInline);
      for (auto attr : {Attribute::NoFree, Attribute::NoSync, Attribute::WillReturn,
                        Attribute::Speculatable, Attribute::NoCallback, Attribute::NoRecurse})
        copy->removeFnAttr(attr);
      copy->setMemoryEffects(MemoryEffects::unknown());
      SmallVector<CallInst *, 8> transfers;
      for (auto &block : *copy)
        for (auto &i : block)
          if (auto *call = dyn_cast<CallInst>(&i);
              call && copies.contains(call->getCalledFunction()))
            transfers.push_back(call);
      for (auto *call : transfers) {
        IRBuilder<> at(call);
        SmallVector<Value *, 8> args;
        for (unsigned n = 0; n + 1 < call->arg_size(); ++n)
          args.push_back(call->getArgOperand(n));
        auto *target = copies[call->getCalledFunction()];
        auto *transfer = at.CreateCall(target, args);
        transfer->setCallingConv(target->getCallingConv());
        transfer->setAttributes(target->getAttributes().removeFnAttributes(ctx));
        transfer->setTailCallKind(CallInst::TCK_MustTail);
        transfer->copyMetadata(*call);
        transfer->setDebugLoc(call->getDebugLoc());
        // The original terminal path may pass through a shared return phi.
        // Returning here preserves it without a live caller frame/error slot.
        auto *block = call->getParent();
        for (auto *successor : successors(block))
          successor->removePredecessor(block);
        SmallVector<Instruction *, 4> old;
        for (auto it = call->getIterator(); it != block->end(); ++it)
          old.push_back(&*it);
        for (auto *i : old)
          i->dropAllReferences();
        for (auto *i : old)
          i->eraseFromParent();
        IRBuilder<>(block).CreateRet(transfer);
      }
      auto fail = module.getOrInsertFunction(
          "kalandra_ordinary_fail_v1",
          FunctionType::get(b.getVoidTy(), {b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty()},
                            false));
      auto *failureFn = cast<llvm::Function>(fail.getCallee());
      failureFn->addFnAttr(Attribute::Cold);
      failureFn->addFnAttr(Attribute::NoReturn);
      failureFn->addFnAttr(Attribute::NoUnwind);
      // Recollect after terminal-transfer rewrites (some old returns vanished).
      returns.clear();
      for (auto &block : *copy)
        if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
          returns.push_back(ret);
      for (auto *ret : returns) {
        if (auto *call = dyn_cast<CallInst>(ret->getReturnValue()); call && call->isMustTailCall())
          continue;
        IRBuilder<> at(ret);
        auto *code = at.CreateLoad(at.getInt64Ty(), fault);
        auto *term = SplitBlockAndInsertIfThen(at.CreateICmpNE(code, at.getInt64(0)), ret, true);
        IRBuilder<> failure(term);
        auto *line = failure.CreateLoad(failure.getInt64Ty(),
                                        failure.CreateConstGEP1_64(failure.getInt64Ty(), fault, 1));
        auto *column = failure.CreateLoad(
            failure.getInt64Ty(), failure.CreateConstGEP1_64(failure.getInt64Ty(), fault, 2));
        failure.CreateCall(fail, {code, line, column});
      }
    }
    for (auto *fn : family) {
      auto *copy = copies[fn];
      valid = valid && !verifyFunction(*copy, &errs());
      if (valid)
        simplify(*copy, true);
      valid = valid && !verifyFunction(*copy, &errs()) &&
              !nativeStackBudgetError(*copy, module.getDataLayout());
    }
    if (!valid) {
      for (auto &item : copies)
        item.second->dropAllReferences();
      for (auto &item : copies)
        item.second->eraseFromParent();
      continue;
    }
    changed = true;
  }
  return changed;
}
} // namespace kalandra
