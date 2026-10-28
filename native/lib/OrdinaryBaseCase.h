#pragma once
#include "NativeStack.h"
#include "OrdinaryTransfer.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// Speculative optimization budget, not a language limit. Standard LLVM
// unrolling preserves checked operation order. Publish only a loop-/storage-free
// result; unknown/nested/large loops keep their original code.
inline void
simplifyOrdinaryBufferedCase(llvm::Function &fn,
                             const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cam;
  ModuleAnalysisManager mam;
  PassBuilder pb;
  pb.registerLoopAnalyses(lam);
  pb.registerFunctionAnalyses(fam);
  pb.registerCGSCCAnalyses(cam);
  pb.registerModuleAnalyses(mam);
  pb.crossRegisterProxies(lam, fam, cam, mam);
  auto &li = fam.getResult<LoopAnalysis>(fn);
  auto &se = fam.getResult<ScalarEvolutionAnalysis>(fn);
  bool bounded = true;
  unsigned expansion = 0;
  for (auto *loop : li.getLoopsInPreorder()) {
    const auto trips = se.getSmallConstantTripCount(loop);
    unsigned instructions = 0;
    for (auto *block : loop->blocks())
      instructions += block->size();
    if (!loop->getSubLoops().empty() || !trips || trips > 128 || instructions > 32 ||
        expansion + trips * instructions > 4096) {
      bounded = false;
      break;
    }
    expansion += trips * instructions;
  }
  if (bounded && expansion) {
    for (auto *loop : li.getLoopsInPreorder()) {
      auto *full =
          MDNode::get(fn.getContext(), MDString::get(fn.getContext(), "llvm.loop.unroll.full"));
      auto *id = MDNode::getDistinct(fn.getContext(), {nullptr, full});
      id->replaceOperandWith(0, id);
      loop->setLoopID(id);
    }
    FunctionPassManager unroll;
    unroll.addPass(LoopUnrollPass(
        LoopUnrollOptions(2, true).setFullUnrollMaxCount(128).setPartial(false).setRuntime(false)));
    unroll.run(fn, fam);
  }
  fam.clear();
  simplify(fn, true);
}
inline bool ordinaryCompactBaseCase(llvm::Function &fn, unsigned originalSize, bool buffered) {
  using namespace llvm;
  DominatorTree emptyDom(fn);
  LoopInfo emptyLoops(emptyDom);
  unsigned size = 0;
  bool returns = false, pure = true;
  for (auto &block : fn)
    for (auto &i : block) {
      ++size;
      returns |= isa<ReturnInst>(i);
      if (isa<AllocaInst>(i) || isa<InvokeInst>(i) || i.isAtomic())
        pure = false;
      if (auto *call = dyn_cast<CallBase>(&i);
          call && (!call->getCalledFunction() || !call->getCalledFunction()->isIntrinsic())) {
        auto *target = call->getCalledFunction();

        const bool permission = target &&
                                target->getName() == "kalandra_ordinary_local_enabled_v1" &&
                                call->arg_empty() && call->getType()->isIntegerTy(64);
        const bool transfer =
            target && target->hasInternalLinkage() &&
            (target->hasFnAttribute("kalandra.ordinary.fallback") ||
             target->getName().contains("ordinary_v1.")) &&
            target->getFunctionType() == fn.getFunctionType() &&
            call->getArgOperand(call->arg_size() - 1) == fn.getArg(fn.arg_size() - 1) &&
            isa<CallInst>(call) && ordinaryTerminalTransfer(*cast<CallInst>(call));
        if (!buffered || (!permission && !transfer))
          pure = false;
      }
    }
  return pure && returns && emptyLoops.empty() && size <= (buffered ? 64u : 32u) &&
         size * 3 <= originalSize && !verifyFunction(fn) &&
         !nativeStackBudgetError(fn, fn.getParent()->getDataLayout());
}
// Fold bounded private-buffer work when LLVM removes every loop/allocation.
// Otherwise try a zero-argument specialization and keep the unrestricted worker
// behind a guaranteed tail transfer. Preserve all checks and replay effects;
// only a compact, materially smaller result is admitted.
inline bool
specializeOrdinaryBaseCases(llvm::Module &module,
                            const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        !fn.hasFnAttribute("kalandra.ordinary.base-case"))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *fn : roots) {
    if (fn->arg_empty() || !fn->getArg(fn->arg_size() - 1)->getType()->isPointerTy())
      continue;
    DominatorTree dom(*fn);
    LoopInfo loops(dom);
    if (loops.empty())
      continue;
    unsigned originalSize = 0;
    for (auto &block : *fn)
      originalSize += block.size();
    if (originalSize < 32)
      continue;
    if (fn->hasFnAttribute("kalandra.ordinary.local-buffers")) {
      ValueToValueMapTy map;
      auto *compact = CloneFunction(fn, map);
      compact->setLinkage(GlobalValue::InternalLinkage);
      compact->setName(fn->getName() + ".compact-candidate");
      simplify(*compact, true);
      simplifyOrdinaryBufferedCase(*compact, simplify);
      if (ordinaryCompactBaseCase(*compact, originalSize, true)) {
        auto name = fn->getName().str();
        auto linkage = fn->getLinkage();
        fn->replaceAllUsesWith(compact);
        fn->eraseFromParent();
        compact->setName(name);
        compact->setLinkage(linkage);
        compact->addFnAttr("kalandra.ordinary.base-case");
        changed = true;
        continue;
      }
      compact->eraseFromParent();
    }
    for (unsigned argument = 0; argument + 1 < fn->arg_size(); ++argument) {
      auto *type = fn->getArg(argument)->getType();
      if (!type->isIntegerTy())
        continue;
      ValueToValueMapTy map;
      auto *empty = CloneFunction(fn, map);
      empty->setName(fn->getName() + ".base");
      empty->setLinkage(GlobalValue::InternalLinkage);
      empty->removeFnAttr("kalandra.ordinary.entry");
      empty->getArg(argument)->replaceAllUsesWith(ConstantInt::get(type, 0));
      simplify(*empty, true);
      if (fn->hasFnAttribute("kalandra.ordinary.local-buffers"))
        simplifyOrdinaryBufferedCase(*empty, simplify);
      if (!ordinaryCompactBaseCase(*empty, originalSize,
                                   fn->hasFnAttribute("kalandra.ordinary.local-buffers"))) {
        empty->eraseFromParent();
        continue;
      }
      ValueToValueMapTy workMap;
      auto *work = CloneFunction(fn, workMap);
      work->setName(fn->getName() + ".work");
      work->setLinkage(GlobalValue::InternalLinkage);
      work->removeFnAttr("kalandra.ordinary.entry");
      work->addFnAttr(Attribute::NoInline);
      // The dispatch proves the outlined worker's argument is nonzero. Carry
      // that fact across its ABI so its cloned entry test is not repeated.
      work->addParamAttr(argument,
                         Attribute::get(module.getContext(), Attribute::Range,
                                        ConstantRange(APInt(type->getIntegerBitWidth(), 1),
                                                      APInt(type->getIntegerBitWidth(), 0))));
      simplify(*work, true);

      fn->deleteBody();
      auto &ctx = module.getContext();
      auto *entry = BasicBlock::Create(ctx, "ordinary.entry", fn);
      auto *base = BasicBlock::Create(ctx, "ordinary.base", fn);
      auto *more = BasicBlock::Create(ctx, "ordinary.work", fn);
      IRBuilder<> b(entry);
      b.CreateCondBr(b.CreateICmpEQ(fn->getArg(argument), ConstantInt::get(type, 0)), base, more);
      SmallVector<Value *, 8> args;
      for (auto &arg : fn->args())
        args.push_back(&arg);
      b.SetInsertPoint(base);
      auto *call = b.CreateCall(empty, args);
      call->setCallingConv(empty->getCallingConv());
      call->setAttributes(empty->getAttributes().removeFnAttributes(ctx));
      b.CreateRet(call);
      if (fn->hasFnAttribute("kalandra.ordinary.local-buffers")) {
        // Keep the runtime query's save area out of the dispatcher's nonzero
        // path. Both outlined branches release the caller frame before entry.
        empty->addFnAttr(Attribute::NoInline);
        call->setTailCallKind(CallInst::TCK_MustTail);
      } else {
        InlineFunctionInfo info;
        if (!InlineFunction(*call, info).isSuccess())
          llvm_unreachable("ordinary base must inline");
        if (empty->use_empty())
          empty->eraseFromParent();
      }
      b.SetInsertPoint(more);
      auto *tail = b.CreateCall(work, args);
      tail->setCallingConv(work->getCallingConv());
      tail->setAttributes(work->getAttributes().removeFnAttributes(ctx));
      tail->setTailCallKind(CallInst::TCK_MustTail);
      b.CreateRet(tail);
      fn->addFnAttr("kalandra.ordinary.base-case");
      simplify(*fn, true);
      changed = true;
      break;
    }
  }
  return changed;
}
} // namespace kalandra
