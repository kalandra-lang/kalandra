#pragma once
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include <functional>

namespace kalandra {
// Keep a checked entry's local, call-free computation outside its packet frame.
// A transfer restarts the original entry with the original arguments. Before
// that transfer only private scratch writes and ordinary reads are permitted:
// no token observation, resource operation or externally visible effect repeats.
inline bool outlineCheckedEntryFrames(
    llvm::Module &module, const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 16> entries;
  for (auto &fn : module)
    if (!fn.isDeclaration() && fn.hasFnAttribute("kalandra.unbound.host-entry") &&
        !fn.hasFnAttribute("kalandra.native.pure-frame") &&
        !fn.hasFnAttribute("kalandra.frameless.entry") &&
        !fn.hasFnAttribute("kalandra.boundary") && fn.getSection().empty() &&
        !fn.hasFnAttribute("kalandra.task.frame") &&
        (fn.getReturnType()->isIntegerTy() || fn.getReturnType()->isFloatingPointTy()) &&
        fn.getCallingConv() == CallingConv::C)
      entries.push_back(&fn);
  bool changed = false;
  for (auto *entry : entries) {
    ValueToValueMapTy workerMap, leafMap;
    auto *worker = CloneFunction(entry, workerMap);
    worker->setName(entry->getName() + ".framed");
    worker->setLinkage(GlobalValue::InternalLinkage);
    worker->removeFnAttr("kalandra.unbound.host-entry");
    worker->removeFnAttr(Attribute::AlwaysInline);
    worker->addFnAttr(Attribute::NoInline);
    auto *leaf = CloneFunction(entry, leafMap);
    leaf->setLinkage(GlobalValue::InternalLinkage);
    SmallVector<Instruction *, 16> transfers;
    auto safe = [&](Instruction &i) {
      if (auto *alloca = dyn_cast<AllocaInst>(&i)) return alloca->isStaticAlloca();
      if (i.isAtomic() || isa<InvokeInst>(i) || isa<IndirectBrInst>(i)) return false;
      if (auto *load = dyn_cast<LoadInst>(&i)) return load->isSimple();
      if (auto *store = dyn_cast<StoreInst>(&i))
        return store->isSimple() && isa<AllocaInst>(getUnderlyingObject(store->getPointerOperand()));
      if (auto *call = dyn_cast<CallBase>(&i)) {
        auto *fn = call->getCalledFunction();
        if (!isa<CallInst>(call) || !fn || !fn->isIntrinsic()) return false;
        switch (fn->getIntrinsicID()) {
        case Intrinsic::lifetime_start: case Intrinsic::lifetime_end:
        case Intrinsic::assume: case Intrinsic::expect: case Intrinsic::expect_with_probability:
        case Intrinsic::smax: case Intrinsic::smin: case Intrinsic::umax: case Intrinsic::umin:
        case Intrinsic::abs: case Intrinsic::bswap: case Intrinsic::bitreverse:
        case Intrinsic::ctpop: case Intrinsic::ctlz: case Intrinsic::cttz:
        case Intrinsic::sadd_with_overflow: case Intrinsic::uadd_with_overflow:
        case Intrinsic::ssub_with_overflow: case Intrinsic::usub_with_overflow:
        case Intrinsic::smul_with_overflow: case Intrinsic::umul_with_overflow: return true;
        default: return false;
        }
      }
      return true;
    };
    for (auto &block : *leaf)
      for (auto &i : block)
        if (!safe(i)) { transfers.push_back(&i); break; }
    for (auto *i : transfers) {
      auto *before = i->getParent();
      before->splitBasicBlock(i->getIterator(), "entry.requires-frame");
      before->getTerminator()->eraseFromParent();
      IRBuilder<> b(before);
      SmallVector<Value *, 12> args;
      for (auto &arg : leaf->args()) args.push_back(&arg);
      auto *call = b.CreateCall(worker, args);
      call->setCallingConv(worker->getCallingConv());
      SmallVector<AttributeSet, 12> argAttributes;
      for (unsigned index = 0; index < worker->arg_size(); ++index)
        argAttributes.push_back(worker->getAttributes().getParamAttrs(index));
      // Return/parameter ABI attributes (notably zeroext Boolean returns) must
      // match the original entry for target lowering of this required tail call.
      call->setAttributes(AttributeList::get(module.getContext(), AttributeSet{},
          worker->getAttributes().getRetAttrs(), argAttributes));
      call->setTailCallKind(CallInst::TCK_MustTail);
      b.CreateRet(call);
    }
    removeUnreachableBlocks(*leaf);
    {
      LoopAnalysisManager loops; FunctionAnalysisManager functions;
      CGSCCAnalysisManager calls; ModuleAnalysisManager modules; PassBuilder passes;
      passes.registerLoopAnalyses(loops); passes.registerFunctionAnalyses(functions);
      passes.registerCGSCCAnalyses(calls); passes.registerModuleAnalyses(modules);
      passes.crossRegisterProxies(loops, functions, calls, modules);
      FunctionPassManager promote;
      promote.addPass(SROAPass(SROAOptions{}));
      promote.run(*leaf, functions);
    }
    simplify(*leaf, true);
    bool frameless = true, hasValuePath = false;
    for (auto &block : *leaf) {
      for (auto &i : block) frameless &= !isa<AllocaInst>(i);
      if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator())) {
        auto *call = dyn_cast_or_null<CallInst>(ret->getPrevNode());
        hasValuePath |= !call || call->getCalledFunction() != worker;
      }
    }
    if (!frameless || !hasValuePath || verifyFunction(*leaf)) {
      leaf->eraseFromParent();
      worker->dropAllReferences(); worker->eraseFromParent();
      continue;
    }
    entry->deleteBody();
    for (auto &arg : leaf->args()) arg.replaceAllUsesWith(entry->getArg(arg.getArgNo()));
    entry->splice(entry->end(), leaf);
    leaf->eraseFromParent();
    // The wrapper has no local storage or ordinary calls. The worker retains
    // its original frame pointer, unwind records, admission and checks.
    entry->addFnAttr("frame-pointer", "none");
    entry->addFnAttr("kalandra.frameless.entry");
    changed = true;
  }
  return changed;
}
} // namespace kalandra
