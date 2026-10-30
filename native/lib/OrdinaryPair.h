#pragma once
#include "NativeStack.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// Experimental two-register success/error ABI. Keep the recoverable v1 entry.
inline bool addOrdinaryPairEntries(llvm::Module &module,
                                   const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  if (module.getDataLayout().getPointerSizeInBits() != 64)
    return false;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.entry") &&
        !fn.hasFnAttribute("kalandra.ordinary.local-buffers") &&
        fn.getName().ends_with("_ordinary_v1") && fn.arg_size() &&
        ((fn.getReturnType()->isIntegerTy() && fn.getReturnType()->getIntegerBitWidth() <= 64) ||
         fn.getReturnType()->isFloatTy() || fn.getReturnType()->isDoubleTy()))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *root : roots) {
    ValueToValueMapTy map;
    auto *flat = CloneFunction(root, map);
    flat->setLinkage(GlobalValue::InternalLinkage);
    flat->setName(root->getName() + ".pair-candidate");
    bool valid = true;
    for (unsigned tries = 0; tries < 32; ++tries) {
      CallInst *next = nullptr;
      unsigned size = 0;
      for (auto &block : *flat)
        for (auto &i : block) {
          ++size;
          if (auto *call = dyn_cast<CallInst>(&i);
              call && (!call->getCalledFunction() || !call->getCalledFunction()->isIntrinsic())) {
            if (call->isInlineAsm() && call->getMetadata("kalandra.ordinary.fixed-convert") &&
                !cast<InlineAsm>(call->getCalledOperand())->hasSideEffects())
              continue;
            auto *callee = call->getCalledFunction();
            if (!callee || callee->isDeclaration() || !callee->getName().contains("ordinary_v1."))
              valid = false;
            else
              next = call;
          }
        }
      if (!valid || size > 4096) {
        valid = false;
        break;
      }
      if (!next)
        break;
      next->setTailCallKind(CallInst::TCK_None);
      InlineFunctionInfo info;
      if (!InlineFunction(*next, info).isSuccess()) {
        valid = false;
        break;
      }
      if (tries == 31)
        valid = false;
    }
    if (!valid) {
      flat->eraseFromParent();
      continue;
    }
    auto &ctx = module.getContext();
    IRBuilder<> begin(&*flat->getEntryBlock().getFirstInsertionPt());
    auto *status = begin.CreateAlloca(begin.getInt64Ty(), nullptr, "ordinary.status");
    begin.CreateStore(begin.getInt64(0), status);
    auto *fault = flat->getArg(flat->arg_size() - 1);
    SmallVector<StoreInst *, 8> codes;
    SmallVector<MemSetInst *, 8> fills;
    for (auto &block : *flat)
      for (auto &i : block)
        if (auto *memory = dyn_cast<MemIntrinsic>(&i)) {
          int64_t offset = 0;
          if (GetPointerBaseWithConstantOffset(memory->getRawDest(), offset,
                                               module.getDataLayout()) == fault &&
              offset < 8) {
            auto *fill = dyn_cast<MemSetInst>(memory);
            auto *length = fill ? dyn_cast<ConstantInt>(fill->getLength()) : nullptr;
            if (offset != 0 || !fill || !length || length->getValue().ult(8))
              valid = false;
            else
              fills.push_back(fill);
          }
        }

    for (auto &block : *flat)
      for (auto &i : block)
        if (auto *store = dyn_cast<StoreInst>(&i)) {
          int64_t offset = 0;
          if (GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset,
                                               module.getDataLayout()) == fault &&
              offset == 0) {
            if (!store->getValueOperand()->getType()->isIntegerTy(64))
              valid = false;
            else
              codes.push_back(store);
          }
        }
    if (!valid) {
      flat->eraseFromParent();
      continue;
    }
    for (auto *store : codes) {
      IRBuilder<> b(store->getNextNode());
      b.CreateStore(store->getValueOperand(), status);
    }
    for (auto *fill : fills) {
      IRBuilder<> b(fill->getNextNode());
      auto *byte = b.CreateZExt(fill->getValue(), b.getInt64Ty());
      b.CreateStore(b.CreateMul(byte, b.getInt64(0x0101010101010101ULL)), status);
    }
    SmallVector<Type *, 8> types;
    SmallVector<AttributeSet, 8> attrs;
    for (auto &arg : flat->args()) {
      types.push_back(arg.getType());
      attrs.push_back(flat->getAttributes().getParamAttrs(arg.getArgNo()));
    }
    auto *pairType = StructType::get(ctx, {begin.getInt64Ty(), begin.getInt64Ty()});
    auto name = root->getName().str();
    name.resize(name.size() - std::string("ordinary_v1").size());
    name += "ordinary_pair_v1";
    auto *pair = llvm::Function::Create(
        FunctionType::get(pairType, types, false),
        module.getTargetTriple().isOSWindows() ? GlobalValue::InternalLinkage : root->getLinkage(),
        name, module);
    pair->setCallingConv(root->getCallingConv());
    pair->setAttributes(
        AttributeList::get(ctx, flat->getAttributes().getFnAttrs(), AttributeSet(), attrs));
    pair->removeFnAttr("kalandra.ordinary.entry");
    pair->addFnAttr("kalandra.ordinary.pair");
    ValueToValueMapTy copied;
    for (auto &arg : flat->args())
      copied[&arg] = pair->getArg(arg.getArgNo());
    SmallVector<ReturnInst *, 8> returns;
    CloneFunctionInto(pair, flat, copied, CloneFunctionChangeType::LocalChangesOnly, returns);
    // CloneFunctionInto copies the old scalar return attributes; the aggregate
    // return deliberately has no scalar sign/zero-extension attributes.
    pair->setAttributes(
        AttributeList::get(ctx, flat->getAttributes().getFnAttrs(), AttributeSet(), attrs));
    pair->removeFnAttr("kalandra.ordinary.entry");
    pair->addFnAttr("kalandra.ordinary.pair");

    for (auto *ret : returns) {
      IRBuilder<> b(ret);
      Value *value = ret->getReturnValue();
      if (value->getType()->isFloatingPointTy())
        value = b.CreateBitCast(value, b.getIntNTy(value->getType()->getPrimitiveSizeInBits()));
      value = b.CreateZExtOrTrunc(value, b.getInt64Ty());
      auto *code = b.CreateLoad(b.getInt64Ty(), copied[status]);
      Value *result = UndefValue::get(pairType);
      result = b.CreateInsertValue(result, value, 0);
      result = b.CreateInsertValue(result, code, 1);
      b.CreateRet(result);
      ret->eraseFromParent();
    }
    flat->eraseFromParent();
    if (verifyFunction(*pair, &errs())) {
      pair->eraseFromParent();
      continue;
    }
    simplify(*pair, true);
    if (verifyFunction(*pair) || nativeStackBudgetError(*pair, module.getDataLayout())) {
      pair->eraseFromParent();
      continue;
    }
    changed = true;
  }
  return changed;
}
} // namespace kalandra
