#pragma once
#include "NativeStack.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <functional>

namespace kalandra {
// A closed, resource-free ordinary entry can report process failure directly.
// There is no per-call token, error buffer or status test on its normal path.
// Recoverable entries remain separate, and managed/task code never selects it.
inline bool addOrdinaryDirectEntries(llvm::Module &module,
                                     const std::function<void(llvm::Function &, bool)> &simplify) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> roots;
  for (auto &fn : module)
    if (fn.hasFnAttribute("kalandra.ordinary.pair") && fn.getName().ends_with("_ordinary_pair_v1"))
      roots.push_back(&fn);
  bool changed = false;
  for (auto *pair : roots) {
    auto name = pair->getName().str();
    name.resize(name.size() - std::string("ordinary_pair_v1").size());
    auto *scalar = module.getFunction(name + "ordinary_v1");
    if (!scalar || pair->arg_empty() ||
        !pair->getArg(pair->arg_size() - 1)->getType()->isPointerTy())
      continue;
    auto &ctx = module.getContext();
    SmallVector<Type *, 8> params;
    SmallVector<AttributeSet, 8> attrs;
    for (unsigned n = 0; n + 1 < pair->arg_size(); ++n) {
      params.push_back(pair->getArg(n)->getType());
      attrs.push_back(pair->getAttributes().getParamAttrs(n));
    }
    auto *direct = llvm::Function::Create(FunctionType::get(scalar->getReturnType(), params, false),
                                          pair->getLinkage(), name + "ordinary_direct_v1", module);
    direct->setCallingConv(pair->getCallingConv());
    IRBuilder<> b(BasicBlock::Create(ctx, "ordinary.direct", direct));
    auto *fault = b.CreateAlloca(ArrayType::get(b.getInt64Ty(), 3), nullptr, "ordinary.cold-fault");
    ValueToValueMapTy map;
    for (unsigned n = 0; n + 1 < pair->arg_size(); ++n)
      map[pair->getArg(n)] = direct->getArg(n);
    map[pair->getArg(pair->arg_size() - 1)] = fault;
    SmallVector<ReturnInst *, 8> returns;
    CloneFunctionInto(direct, pair, map, CloneFunctionChangeType::LocalChangesOnly, returns);
    b.CreateBr(cast<BasicBlock>(map[&pair->getEntryBlock()]));
    direct->setAttributes(AttributeList::get(ctx, pair->getAttributes().getFnAttrs(),
                                             scalar->getAttributes().getRetAttrs(), attrs));
    direct->removeFnAttr("kalandra.ordinary.pair");
    direct->addFnAttr("kalandra.ordinary.direct");
    for (auto attr : {Attribute::NoFree, Attribute::NoSync, Attribute::WillReturn,
                      Attribute::Speculatable, Attribute::NoCallback, Attribute::NoRecurse})
      direct->removeFnAttr(attr);
    direct->setMemoryEffects(MemoryEffects::unknown());
    auto fail = module.getOrInsertFunction(
        "kalandra_ordinary_fail_v1",
        FunctionType::get(b.getVoidTy(), {b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty()}, false));
    if (auto *fn = dyn_cast<llvm::Function>(fail.getCallee())) {
      fn->addFnAttr(Attribute::Cold);
      fn->addFnAttr(Attribute::NoReturn);
      fn->addFnAttr(Attribute::NoUnwind);
    }
    for (auto *ret : returns) {
      IRBuilder<> at(ret);
      auto *code = at.CreateExtractValue(ret->getReturnValue(), 1);
      Value *bits = at.CreateExtractValue(ret->getReturnValue(), 0);
      Value *value = nullptr;
      if (direct->getReturnType()->isFloatingPointTy()) {
        bits = at.CreateTruncOrBitCast(
            bits, at.getIntNTy(direct->getReturnType()->getPrimitiveSizeInBits()));
        value = at.CreateBitCast(bits, direct->getReturnType());
      } else
        value = at.CreateTruncOrBitCast(bits, direct->getReturnType());
      auto *finish = at.CreateRet(value);
      ret->eraseFromParent();
      IRBuilder<> condition(finish);
      auto *term = SplitBlockAndInsertIfThen(condition.CreateICmpNE(code, condition.getInt64(0)),
                                             finish, true);
      IRBuilder<> failure(term);
      auto *line = failure.CreateLoad(failure.getInt64Ty(),
                                      failure.CreateConstGEP1_64(failure.getInt8Ty(), fault, 8));
      auto *column = failure.CreateLoad(failure.getInt64Ty(),
                                        failure.CreateConstGEP1_64(failure.getInt8Ty(), fault, 16));
      failure.CreateCall(fail, {code, line, column});
    }
    if (verifyFunction(*direct, &errs())) {
      direct->eraseFromParent();
      continue;
    }
    simplify(*direct, true);
    if (verifyFunction(*direct, &errs()) ||
        nativeStackBudgetError(*direct, module.getDataLayout())) {
      direct->eraseFromParent();
      continue;
    }
    changed = true;
  }
  return changed;
}
} // namespace kalandra
