#pragma once
#include "NativeStack.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>
namespace kalandra {
// Inlining a small-domain body may turn its terminal musttail replay into a
// call followed by lifetime markers and a return phi. Restore the guaranteed
// frame-releasing transfer only after verifying that exact forwarding shape.
inline bool restoreOrdinaryReplayTransfers(llvm::Function &fn) {
  using namespace llvm;
  SmallVector<WeakTrackingVH, 8> calls;
  for (auto &block : fn)
    for (auto &i : block)
      if (auto *call = dyn_cast<CallInst>(&i);
          call && call->getMetadata("kalandra.ordinary.range-replay"))
        calls.push_back(call);
  for (auto &handle : calls) {
    auto *call = dyn_cast_or_null<CallInst>(handle);
    if (!call)
      continue;
    auto *block = call->getParent();
    Value *forward = call;
    auto at = std::next(call->getIterator());
    SmallPtrSet<BasicBlock *, 8> seen;
    bool returns = false;
    while (seen.insert(block).second) {
      bool clean = true;
      for (; at != block->end() && !at->isTerminator(); ++at) {
        auto *intrinsic = dyn_cast<IntrinsicInst>(&*at);
        if (!intrinsic || intrinsic->getIntrinsicID() != Intrinsic::lifetime_end)
          clean = false;
      }
      if (!clean)
        break;
      if (auto *ret = dyn_cast<ReturnInst>(block->getTerminator())) {
        returns = ret->getReturnValue() == forward;
        break;
      }
      auto *branch = dyn_cast<UncondBrInst>(block->getTerminator());
      if (!branch)
        break;
      auto *next = branch->getSuccessor(0);
      for (auto &phi : next->phis())
        if (phi.getIncomingValueForBlock(block) == forward)
          forward = &phi;
      block = next;
      at = block->getFirstNonPHIIt();
    }
    if (!returns)
      return false;
    block = call->getParent();
    if (auto *branch = dyn_cast<UncondBrInst>(block->getTerminator()))
      branch->getSuccessor(0)->removePredecessor(block);
    auto cursor = std::next(call->getIterator());
    while (cursor != block->end())
      cursor = cursor->eraseFromParent();
    call->setTailCallKind(CallInst::TCK_MustTail);
    ReturnInst::Create(fn.getContext(), call, block);
  }
  return true;
}
// Version only expensive, already-proven private reductions. The small-domain
// argument contract is checked at entry, never inferred from benchmark inputs.
// Require many wide-constant input tests and material simplification so tiny
// entries do not gain another dispatch. The unrestricted ordinary ABI remains.
inline bool specializeOrdinaryInputs(llvm::Module &module,
                                     const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        fn.hasFnAttribute("kalandra.ordinary.reduction") &&
        !fn.hasFnAttribute("kalandra.ordinary.small-input") && fn.use_empty())
      roots.push_back(&fn);
  bool changed = false;
  for (auto *original : roots) {
    SmallVector<unsigned, 8> arguments;
    for (auto &arg : original->args())
      if (arg.getType()->isIntegerTy(64))
        arguments.push_back(arg.getArgNo());
    if (arguments.empty())
      continue;
    DominatorTree dom(*original);
    LoopInfo loops(dom);
    unsigned checks = 0, before = 0;
    for (auto &block : *original)
      for (auto &i : block) {
        ++before;
        if (loops.getLoopFor(&block))
          continue;
        if (auto *cmp = dyn_cast<ICmpInst>(&i); cmp && isa<Argument>(cmp->getOperand(0)))
          if (auto *c = dyn_cast<ConstantInt>(cmp->getOperand(1));
              c && c->getBitWidth() == 64 && c->getValue().ugt(0xffffffffULL))
            ++checks;
      }
    if (checks < 16)
      continue;
    ValueToValueMapTy map;
    auto *small = CloneFunction(original, map);
    auto &ctx = module.getContext();
    small->setName(original->getName() + ".small-candidate");
    small->setLinkage(GlobalValue::InternalLinkage);
    for (auto n : arguments)
      small->addParamAttr(
          n, Attribute::get(ctx, Attribute::Range, ConstantRange(APInt(64, 0), APInt(64, 65536))));
    simplify(*small, true);
    unsigned after = 0;
    for (auto &block : *small)
      after += block.size();
    if (after + 32 > before || after * 10 > before * 9 || verifyFunction(*small)) {
      small->eraseFromParent();
      continue;
    }
    auto *wrapper =
        llvm::Function::Create(original->getFunctionType(), GlobalValue::InternalLinkage,
                               original->getName() + ".input-dispatch", module);
    wrapper->setAttributes(original->getAttributes());
    wrapper->setCallingConv(original->getCallingConv());
    IRBuilder<> b(BasicBlock::Create(ctx, "ordinary.inputs", wrapper));
    Value *bits = b.getInt64(0);
    for (auto n : arguments)
      bits = b.CreateOr(bits, wrapper->getArg(n));
    auto *fast = BasicBlock::Create(ctx, "ordinary.small", wrapper),
         *wide = BasicBlock::Create(ctx, "ordinary.full", wrapper);
    b.CreateCondBr(b.CreateICmpULT(bits, b.getInt64(65536)), fast, wide);
    SmallVector<Value *, 8> args;
    for (auto &arg : wrapper->args())
      args.push_back(&arg);
    b.SetInsertPoint(fast);
    auto *call = b.CreateCall(small, args);
    call->setCallingConv(small->getCallingConv());
    call->setAttributes(small->getAttributes().removeFnAttributes(ctx));
    b.CreateRet(call);
    InlineFunctionInfo info;
    if (!InlineFunction(*call, info).isSuccess())
      llvm_unreachable("small ordinary graph must inline");
    if (small->use_empty())
      small->eraseFromParent();
    b.SetInsertPoint(wide);
    auto *tail = b.CreateCall(original, args);
    tail->setCallingConv(original->getCallingConv());
    tail->setAttributes(original->getAttributes().removeFnAttributes(ctx));
    tail->setTailCallKind(CallInst::TCK_MustTail);
    b.CreateRet(tail);
    if (!restoreOrdinaryReplayTransfers(*wrapper)) {
      wrapper->eraseFromParent();
      continue;
    }
    simplify(*wrapper, true);
    if (verifyFunction(*wrapper) || nativeStackBudgetError(*wrapper, module.getDataLayout())) {
      wrapper->eraseFromParent();
      continue;
    }
    auto name = original->getName().str();
    auto linkage = original->getLinkage();
    original->setName(name + ".full-input");
    original->setLinkage(GlobalValue::InternalLinkage);
    original->removeFnAttr("kalandra.ordinary.entry");
    original->addFnAttr(Attribute::NoInline);
    wrapper->setName(name);
    wrapper->setLinkage(linkage);
    wrapper->addFnAttr("kalandra.ordinary.small-input");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
