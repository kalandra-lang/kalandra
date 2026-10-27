#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// A general base-case version for scalar ordinary entries. Only a zero argument
// that eliminates every loop and most instructions qualifies; the entire body
// is specialized, retaining checks/evaluation that precede its base-case return.
inline bool
specializeOrdinaryBaseCases(llvm::Module &module,
                            const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        !fn.hasFnAttribute("kalandra.ordinary.local-buffers") &&
        !fn.hasFnAttribute("kalandra.ordinary.base-case"))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *fn : roots) {
    DominatorTree dom(*fn);
    LoopInfo loops(dom);
    if (loops.empty())
      continue;
    unsigned originalSize = 0;
    for (auto &block : *fn)
      originalSize += block.size();
    if (originalSize < 32)
      continue;
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
      DominatorTree emptyDom(*empty);
      LoopInfo emptyLoops(emptyDom);
      unsigned size = 0;
      bool returns = false, pure = true;
      for (auto &block : *empty)
        for (auto &i : block) {
          ++size;
          returns |= isa<ReturnInst>(i);
          if (isa<AllocaInst>(i) || isa<InvokeInst>(i) || i.isAtomic())
            pure = false;
          if (auto *call = dyn_cast<CallBase>(&i);
              call && (!call->getCalledFunction() || !call->getCalledFunction()->isIntrinsic()))
            pure = false;
        }
      if (!pure || !returns || !emptyLoops.empty() || size > 32 || size * 3 > originalSize ||
          verifyFunction(*empty)) {
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
      InlineFunctionInfo info;
      if (!InlineFunction(*call, info).isSuccess())
        llvm_unreachable("ordinary base must inline");
      if (empty->use_empty())
        empty->eraseFromParent();
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
