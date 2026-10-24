#pragma once
#include "../runtime/Runtime.h"
#include "NativeStack.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

namespace kalandra {
// Called only for a closed, unbound scalar recursive-context worker. An
// additional proof restricts this version to direct self calls, depth-free
// fault/poll helpers and memory-free intrinsics. No source/helper callback can
// observe invocation depth/status while they are carried through the private ABI.
inline void specializeRecursiveDepth(llvm::Function &worker, llvm::Function *poll,
                                     llvm::ArrayRef<llvm::Function *> bookkeeping,
                                     void (*simplify)(llvm::Function &)) {
  using namespace llvm;
  auto &module = *worker.getParent();
  auto &context = module.getContext();
  DominatorTree originalDominators(worker);
  LoopInfo loops(originalDominators);
  if (!loops.empty())
    return; // Tail loops already have a separate SSA-depth proof.
  auto *fault = module.getFunction("kalandra_fault_v1");
  for (auto &block : worker)
    for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction)) {
        auto *callee = call->getCalledFunction();
        if (callee == &worker || callee == poll || callee == fault ||
            llvm::is_contained(bookkeeping, callee))
          continue;
        if (callee && callee->isIntrinsic() && !call->mayReadOrWriteMemory())
          continue;
        return;
      }
  auto *i64 = Type::getInt64Ty(context);
  SmallVector<Type *, 12> parameters(worker.getFunctionType()->params());
  parameters.push_back(i64);
  auto *resultType = StructType::get(worker.getReturnType(), Type::getInt1Ty(context));
  auto *candidate = Function::Create(
      FunctionType::get(resultType, parameters, false),
      GlobalValue::InternalLinkage, worker.getName() + ".depth", module);
  ValueToValueMapTy values;
  values[&worker] = &worker;
  for (auto &argument : worker.args())
    values[&argument] = candidate->getArg(argument.getArgNo());
  SmallVector<ReturnInst *, 8> returns;
  CloneFunctionInto(candidate, &worker, values, CloneFunctionChangeType::LocalChangesOnly, returns);
  candidate->setLinkage(GlobalValue::InternalLinkage);
  candidate->addFnAttr(Attribute::NoInline);
  candidate->getArg(candidate->arg_size() - 1)->setName("recursive.depth");
  SmallVector<CallInst *, 16> inlineCalls;
  for (auto &block : *candidate)
    for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction);
          call && llvm::is_contained(bookkeeping, call->getCalledFunction()))
        inlineCalls.push_back(call);
  bool safe = true;
  for (auto *call : inlineCalls) {
    InlineFunctionInfo info;
    safe &= InlineFunction(*call, info).isSuccess();
  }
  auto *invocation = candidate->getArg(worker.arg_size() - 2);
  SmallVector<Instruction *, 16> accesses;
  SmallVector<CallInst *, 8> selfCalls;
  for (auto &block : *candidate)
    for (auto &instruction : block) {
      Value *address = nullptr;
      if (auto *load = dyn_cast<LoadInst>(&instruction))
        address = load->getPointerOperand();
      if (auto *store = dyn_cast<StoreInst>(&instruction))
        address = store->getPointerOperand();
      if (auto *gep = dyn_cast_or_null<GetElementPtrInst>(address);
          gep && gep->getPointerOperand() == invocation && gep->getNumIndices() == 1)
        if (auto *offset = dyn_cast<ConstantInt>(gep->getOperand(1));
            offset && offset->getZExtValue() == runtime::InvocationABI::depth)
          accesses.push_back(&instruction);
      if (auto *call = dyn_cast<CallInst>(&instruction)) {
        auto *callee = call->getCalledFunction();
        if (callee == &worker)
          selfCalls.push_back(call);
        else
          safe &= callee == poll || callee == fault ||
                  (callee && callee->isIntrinsic() &&
                   (!call->mayReadOrWriteMemory() || callee->getIntrinsicID() == Intrinsic::stacksave));
      }
    }
  // The first poll observes any already-faulted entry. Thereafter the only
  // status changes are explicit local faults and self-call status results.
  // Unbound, exclusive ownership excludes asynchronous cancellation/observers.
  CallInst *entryPoll = nullptr;
  for (auto &instruction : candidate->getEntryBlock())
    if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction() == poll) {
      if (entryPoll)
        safe = false;
      entryPoll = call;
    }
  DominatorTree entryDominators(*candidate);
  SmallVector<CallInst *, 16> statusCalls;
  for (auto &block : *candidate)
    for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction);
          call && call->getCalledFunction() == poll && call != entryPoll) {
        safe &= entryPoll && entryDominators.dominates(entryPoll, call);
        for (auto *user : call->users()) {
          auto *comparison = dyn_cast<ICmpInst>(user);
          auto *constant = comparison ? dyn_cast<ConstantInt>(comparison->getOperand(1)) : nullptr;
          safe &= comparison && comparison->isEquality() && constant && constant->isZero() &&
                  comparison->getOperand(0) == call;
        }
        statusCalls.push_back(call);
      }
  safe &= entryPoll != nullptr;
  if (safe && !accesses.empty() && !selfCalls.empty()) {
    IRBuilder<> b(&*candidate->getEntryBlock().getFirstInsertionPt());
    auto *slot = b.CreateAlloca(i64, nullptr, "recursive.depth.slot");
    auto *ok = b.CreateAlloca(b.getInt1Ty(), nullptr, "recursive.status.slot");
    b.CreateStore(candidate->getArg(candidate->arg_size() - 1), slot);
    b.CreateStore(b.getTrue(), ok);
    IRBuilder<> afterEntry(entryPoll->getNextNode());
    afterEntry.CreateStore(afterEntry.CreateICmpEQ(entryPoll, afterEntry.getInt64(0)), ok);
    for (auto &block : *candidate)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction() == fault) {
          IRBuilder<> at(call);
          at.CreateStore(at.getFalse(), ok);
        }
    for (auto *access : accesses)
      if (auto *load = dyn_cast<LoadInst>(access))
        load->setOperand(0, slot);
      else
        cast<StoreInst>(access)->setOperand(1, slot);
    for (auto *call : selfCalls) {
      IRBuilder<> at(call);
      SmallVector<Value *, 12> arguments(call->args());
      arguments.push_back(at.CreateLoad(i64, slot));
      auto *replacement = at.CreateCall(candidate, arguments);
      replacement->setCallingConv(call->getCallingConv());
      auto *value = at.CreateExtractValue(replacement, 0);
      auto *status = at.CreateExtractValue(replacement, 1);
      at.CreateStore(at.CreateAnd(at.CreateLoad(at.getInt1Ty(), ok), status), ok);
      call->replaceAllUsesWith(value);
      call->eraseFromParent();
    }
    for (auto *call : statusCalls) {
      IRBuilder<> at(call);
      auto *status = at.CreateSelect(at.CreateLoad(at.getInt1Ty(), ok), at.getInt64(0), at.getInt64(1));
      call->replaceAllUsesWith(status);
      call->eraseFromParent();
    }
    SmallVector<ReturnInst *, 8> exits;
    for (auto &block : *candidate)
      if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        exits.push_back(ret);
    for (auto *ret : exits) {
      IRBuilder<> at(ret);
      Value *value = at.CreateInsertValue(PoisonValue::get(resultType), ret->getReturnValue(), 0);
      value = at.CreateInsertValue(value, at.CreateLoad(at.getInt1Ty(), ok), 1);
      at.CreateRet(value);
      ret->eraseFromParent();
    }
    DominatorTree dominators(*candidate);
    PromoteMemToReg({slot, ok}, dominators);
    simplify(*candidate);
    // Expand one frozen private level, never recursively growing the snapshot.
    // All copied logical/physical admission and fault/status paths remain.
    ValueToValueMapTy snapshotValues, expandedValues;
    snapshotValues[candidate] = candidate;
    expandedValues[candidate] = candidate;
    auto *snapshot = CloneFunction(candidate, snapshotValues);
    auto *expanded = CloneFunction(candidate, expandedValues);
    SmallVector<CallInst *, 4> calls;
    for (auto &block : *expanded)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && call->getCalledFunction() == candidate)
          calls.push_back(call);
    bool expandedAll = !calls.empty() && calls.size() <= 4;
    if (expandedAll)
      for (auto *call : calls) {
        call->setCalledFunction(snapshot);
        InlineFunctionInfo info;
        expandedAll &= InlineFunction(*call, info).isSuccess();
      }
    simplify(*expanded);
    const bool expandedFits = expandedAll &&
                              !nativeStackBudgetError(*expanded, module.getDataLayout());
    if (expandedFits) {
      const auto linkage = candidate->getLinkage();
      candidate->deleteBody();
      candidate->setLinkage(linkage);
      for (auto &argument : expanded->args())
        argument.replaceAllUsesWith(candidate->getArg(argument.getArgNo()));
      candidate->splice(candidate->begin(), expanded);
    }
    expanded->dropAllReferences();
    expanded->eraseFromParent();
    snapshot->dropAllReferences();
    snapshot->eraseFromParent();
    // A small existing-signature bridge captures/restores the outer memory
    // state. It never admits a logical slot or owns the outer stack lease.
    // Recursive edges bypass it; every actual recursive frame still checks SP.
    auto *bridge = Function::Create(worker.getFunctionType(), GlobalValue::InternalLinkage,
                                    worker.getName() + ".depth.bridge", module);
    IRBuilder<> bridgeBuilder(BasicBlock::Create(context, "entry", bridge));
    auto *outer = bridge->getArg(bridge->arg_size() - 2);
    auto *depthAddress = bridgeBuilder.CreateConstGEP1_64(bridgeBuilder.getInt8Ty(), outer,
                                                        runtime::InvocationABI::depth);
    auto *depth = bridgeBuilder.CreateLoad(i64, depthAddress);
    SmallVector<Value *, 12> arguments;
    for (auto &argument : bridge->args())
      arguments.push_back(&argument);
    arguments.push_back(depth);
    auto *result = bridgeBuilder.CreateCall(candidate, arguments);
    bridgeBuilder.CreateStore(depth, depthAddress);
    bridgeBuilder.CreateRet(bridgeBuilder.CreateExtractValue(result, 0));
    if (expandedFits && !nativeStackBudgetError(*candidate, module.getDataLayout()) &&
        !nativeStackBudgetError(*bridge, module.getDataLayout(), false, 16 * 1024 - 512)) {
      const auto linkage = worker.getLinkage();
      worker.deleteBody();
      worker.setLinkage(linkage);
      for (auto &argument : bridge->args())
        argument.replaceAllUsesWith(worker.getArg(argument.getArgNo()));
      worker.splice(worker.begin(), bridge);
      candidate = nullptr; // Adopted; no public signature or source identity changes.
    }
    bridge->eraseFromParent();
  }
  if (candidate) {
    candidate->dropAllReferences();
    candidate->eraseFromParent();
  }
}
} // namespace kalandra
