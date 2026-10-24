#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <array>
#include <unordered_map>

namespace kalandra {
// Darwin's pinned backend does not reliably preserve stack argument packets
// under callee-pop conventions in unoptimized code. Keep those signatures on
// caller-pop native calls; explicit aggregate outputs still permit matching-
// signature musttail calls. Register-only calls use tailcc, including differing
// source parameter lists. Other qualified targets use tailcc throughout.
inline llvm::CallingConv::ID nativeInternalCallingConvention(llvm::FunctionType *type) {
#if defined(__APPLE__) && defined(__aarch64__)
  unsigned general = 0, floating = 0;
  for (auto *parameter : type->params()) {
    if (parameter->isFloatingPointTy() || parameter->isVectorTy()) ++floating;
    else if (parameter->isPointerTy() || parameter->isIntegerTy(64) ||
             (parameter->isIntegerTy() && parameter->getIntegerBitWidth() < 64)) ++general;
    else return llvm::CallingConv::C;
  }
  if (general > 8 || floating > 8) return llvm::CallingConv::C;
#else
  (void)type;
#endif
  return llvm::CallingConv::Tail;
}
// Resolver construction is finalized only after task-owned callable entries
// have been emitted. The registry stores full lowered signatures, never just
// an argument count or an unchecked fallback code pointer.
inline void populateNativeCallResolver(
    llvm::Function &fn,
    llvm::ArrayRef<std::pair<llvm::Function *, llvm::Function *>> targets) {
  using namespace llvm;
  auto &module = *fn.getParent();
  auto &context = module.getContext();
  auto *pointer = PointerType::getUnqual(context);
  auto *rowType = StructType::get(pointer, pointer);
  SmallVector<Constant *, 8> rows;
  for (auto [entry, body] : targets)
    rows.push_back(ConstantStruct::get(rowType, {entry, body}));
  auto *tableType = ArrayType::get(rowType, rows.size());
  auto *table = new GlobalVariable(module, tableType, true, GlobalValue::InternalLinkage,
                                   ConstantArray::get(tableType, rows), "call.targets");
  fn.deleteBody();
  fn.setLinkage(GlobalValue::InternalLinkage);
  IRBuilder<> b(BasicBlock::Create(context, "entry", &fn));
  auto *loop = BasicBlock::Create(context, "search", &fn);
  auto *probe = BasicBlock::Create(context, "probe", &fn);
  auto *next = BasicBlock::Create(context, "next", &fn);
  auto *found = BasicBlock::Create(context, "found", &fn);
  auto *absent = BasicBlock::Create(context, "absent", &fn);
  auto *entry = b.GetInsertBlock();
  b.CreateBr(loop);
  b.SetInsertPoint(loop);
  auto *index = b.CreatePHI(b.getInt64Ty(), 2);
  index->addIncoming(b.getInt64(0), entry);
  b.CreateCondBr(b.CreateICmpULT(index, b.getInt64(rows.size())), probe, absent);
  b.SetInsertPoint(probe);
  auto *row = b.CreateGEP(tableType, table, {b.getInt64(0), index});
  auto *code = b.CreateLoad(pointer, b.CreateStructGEP(rowType, row, 0));
  b.CreateCondBr(b.CreateICmpEQ(code, fn.getArg(0)), found, next);
  b.SetInsertPoint(next);
  auto *increment = b.CreateAdd(index, b.getInt64(1));
  b.CreateBr(loop);
  index->addIncoming(increment, next);
  b.SetInsertPoint(found);
  b.CreateRet(b.CreateLoad(pointer, b.CreateStructGEP(rowType, row, 1)));
  b.SetInsertPoint(absent);
  b.CreateRet(ConstantPointerNull::get(pointer));
}
inline void finalizeNativeCallResolvers(llvm::Module &module) {
  using namespace llvm;
  SmallVector<llvm::Function *, 8> resolvers, plans;
  for (auto &fn : module) {
    if (fn.getMetadata("kalandra.call.signature")) resolvers.push_back(&fn);
    if (!fn.isDeclaration() && fn.getName().starts_with("kalandra_callable_plan_"))
      plans.push_back(&fn);
  }
  auto &context = module.getContext();
  auto *pointer = PointerType::getUnqual(context);
  for (auto *resolver : resolvers) {
    auto *signature = cast<llvm::Function>(cast<ValueAsMetadata>(
        resolver->getMetadata("kalandra.call.signature")->getOperand(0))->getValue());
    auto *sourceType = signature->getFunctionType();
    const bool aggregate = sourceType->getReturnType()->isAggregateType();
    SmallVector<Type *, 12> parameters(sourceType->params());
    parameters.push_back(pointer); // Checked planned entries receive Invocation*.
    auto *checkedType = FunctionType::get(sourceType->getReturnType(), parameters, false);
    parameters.append({pointer, Type::getInt64Ty(context)});
    if (aggregate) parameters.insert(parameters.begin(), pointer);
    auto *internalType = FunctionType::get(aggregate ? Type::getVoidTy(context) :
                                          sourceType->getReturnType(), parameters, false);
    SmallVector<std::pair<llvm::Function *, llvm::Function *>, 8> targets;
    auto *registry = resolver->getMetadata("kalandra.call.targets");
    for (unsigned n = 0; n < registry->getNumOperands(); n += 2)
      targets.emplace_back(cast<llvm::Function>(cast<ValueAsMetadata>(registry->getOperand(n))->getValue()),
                           cast<llvm::Function>(cast<ValueAsMetadata>(registry->getOperand(n + 1))->getValue()));
    for (auto *plan : plans) {
      if (plan->getFunctionType() != checkedType) continue;
      auto *adapter = llvm::Function::Create(internalType, GlobalValue::InternalLinkage,
                                            plan->getName() + ".native", module);
      adapter->setCallingConv(nativeInternalCallingConvention(internalType));
      adapter->setUWTableKind(UWTableKind::Sync);
      adapter->addFnAttr("frame-pointer", "all");
      IRBuilder<> b(BasicBlock::Create(context, "entry", adapter));
      SmallVector<Value *, 12> args;
      for (unsigned n = unsigned(aggregate); n < adapter->arg_size() - 2; ++n)
        args.push_back(adapter->getArg(n));
      auto *invocation = adapter->getArg(adapter->arg_size() - 3);
      auto *value = b.CreateCall(plan, args);
      auto poll = module.getOrInsertFunction("kalandra_poll_v1", b.getInt64Ty(), pointer);
      auto *ready = BasicBlock::Create(context, "ready", adapter);
      auto *failed = BasicBlock::Create(context, "failed", adapter);
      b.CreateCondBr(b.CreateICmpEQ(b.CreateCall(poll, {invocation}), b.getInt64(0)), ready, failed);
      b.SetInsertPoint(failed);
      b.CreateCall(module.getOrInsertFunction("kalandra_source_abort_v2", b.getVoidTy()));
      b.CreateUnreachable();
      b.SetInsertPoint(ready);
      if (aggregate) b.CreateStore(value, adapter->getArg(0));
      if (aggregate || value->getType()->isVoidTy()) b.CreateRetVoid();
      else b.CreateRet(value);
      targets.emplace_back(plan, adapter);
    }
    populateNativeCallResolver(*resolver, targets);
    resolver->setMetadata("kalandra.call.signature", nullptr);
    resolver->setMetadata("kalandra.call.targets", nullptr);
    signature->eraseFromParent();
  }
}
// Run after composing every execution engine that can produce source values.
void optimizeNativeCalls(std::unique_ptr<llvm::Module> &module);
// Keep call-free return paths outside a recursive worker's physical frame.
// This is partial inlining by control flow, independent of source numeric type,
// recursion depth or benchmark input. The slow region keeps its original body
// and guards; the wrapper is a native leaf or a guaranteed tail transfer.
inline void outlineNativeLeafPaths(llvm::Module &module,
                                   void (*simplify)(llvm::Function &, bool)) {
  using namespace llvm;
  SmallVector<llvm::Function *, 16> candidates;
  for (auto &fn : module)
    if (!fn.isDeclaration() && fn.getName().ends_with(".body.clear") &&
        !fn.hasFnAttribute(Attribute::AlwaysInline))
      candidates.push_back(&fn);
  for (auto *fn : candidates) {
    auto *entry = &fn->getEntryBlock();
    auto *branch = dyn_cast<CondBrInst>(entry->getTerminator());
    if (!branch || branch->getSuccessor(0) == branch->getSuccessor(1)) continue;
    bool fixed = true, recursive = false;
    for (auto &block : *fn)
      for (auto &instruction : block) {
        fixed &= !isa<AllocaInst>(instruction);
        if (auto *call = dyn_cast<CallInst>(&instruction))
          recursive |= call->getCalledFunction() == fn && !call->isMustTailCall();
      }
    if (!fixed || !recursive) continue;
    auto pure = [](Instruction &instruction) {
      return !instruction.mayReadOrWriteMemory() && !isa<CallBase>(instruction) &&
             !isa<AllocaInst>(instruction);
    };
    bool entryPure = true;
    for (auto &instruction : *entry)
      if (!instruction.isTerminator()) entryPure &= pure(instruction);
    if (!entryPure) continue;
    int leafIndex = -1;
    SmallPtrSet<BasicBlock *, 32> region;
    for (int side = 0; side != 2 && leafIndex < 0; ++side) {
      region.clear();
      SmallPtrSet<BasicBlock *, 32> active;
      auto visit = [&](auto &&self, BasicBlock *block) -> bool {
        if (block == entry || active.contains(block)) return false;
        if (region.contains(block)) return true;
        active.insert(block);
        for (auto &instruction : *block)
          if (!instruction.isTerminator() && !pure(instruction)) return false;
        auto *term = block->getTerminator();
        if (!isa<ReturnInst>(term) && !isa<CondBrInst>(term) &&
            !isa<UncondBrInst>(term) && !isa<SwitchInst>(term)) return false;
        for (auto *next : successors(block))
          if (!self(self, next)) return false;
        active.erase(block);
        region.insert(block);
        return true;
      };
      if (!visit(visit, branch->getSuccessor(side))) continue;
      auto available = [&](Value *value) {
        auto *instruction = dyn_cast<Instruction>(value);
        return !instruction || instruction->getParent() == entry ||
               region.contains(instruction->getParent());
      };
      bool valid = true;
      for (auto *block : region)
        for (auto &instruction : *block)
          if (auto *phi = dyn_cast<PHINode>(&instruction)) {
            for (unsigned n = 0; n != phi->getNumIncomingValues(); ++n)
              if (phi->getIncomingBlock(n) == entry || region.contains(phi->getIncomingBlock(n)))
                valid &= available(phi->getIncomingValue(n));
          } else
            for (auto &operand : instruction.operands()) valid &= available(operand.get());
      if (valid) leafIndex = side;
    }
    if (leafIndex < 0) continue;
    ValueToValueMapTy cloned;
    cloned[fn] = fn; // Recursive edges return through the thin wrapper.
    auto *worker = CloneFunction(fn, cloned);
    worker->setName(fn->getName() + ".branch");
    worker->setLinkage(GlobalValue::InternalLinkage);
    worker->removeFnAttr(Attribute::AlwaysInline);
    worker->addFnAttr(Attribute::NoInline);
    auto *workerBranch = cast<CondBrInst>(worker->getEntryBlock().getTerminator());
    auto *leaf = workerBranch->getSuccessor(leafIndex);
    auto *slow = workerBranch->getSuccessor(1 - leafIndex);
    SmallPtrSet<BasicBlock *, 32> workerRegion;
    for (auto *block : region) workerRegion.insert(cast<BasicBlock>(cloned[block]));
    fn->deleteBody();
    fn->setLinkage(GlobalValue::InternalLinkage);
    fn->removeFnAttr(Attribute::NoInline);
    fn->addFnAttr(Attribute::AlwaysInline);
    auto *thinEntry = BasicBlock::Create(module.getContext(), "entry", fn);
    IRBuilder<> b(thinEntry);
    ValueToValueMapTy values;
    values[&worker->getEntryBlock()] = thinEntry;
    for (auto &arg : worker->args()) values[&arg] = fn->getArg(arg.getArgNo());
    for (auto &instruction : worker->getEntryBlock()) {
      if (instruction.isTerminator()) break;
      auto *copy = instruction.clone();
      b.Insert(copy);
      values[&instruction] = copy;
      RemapInstruction(copy, values, RF_IgnoreMissingLocals);
    }
    for (auto *block : workerRegion)
      values[block] = CloneBasicBlock(block, values, ".leaf", fn);
    for (auto *block : workerRegion) {
      auto *copy = cast<BasicBlock>(values[block]);
      for (auto &instruction : *copy) {
        if (auto *phi = dyn_cast<PHINode>(&instruction))
          for (unsigned n = phi->getNumIncomingValues(); n-- > 0;)
            if (phi->getIncomingBlock(n) != &worker->getEntryBlock() &&
                !workerRegion.contains(phi->getIncomingBlock(n)))
              phi->removeIncomingValue(n, false);
        RemapInstruction(&instruction, values, RF_IgnoreMissingLocals);
      }
    }
    auto *outlined = BasicBlock::Create(module.getContext(), "branch", fn);
    auto *fast = cast<BasicBlock>(values[leaf]);
    auto *condition = MapValue(workerBranch->getCondition(), values, RF_IgnoreMissingLocals);
    b.CreateCondBr(condition, leafIndex == 0 ? fast : outlined,
                   leafIndex == 0 ? outlined : fast);
    b.SetInsertPoint(outlined);
    SmallVector<Value *, 12> args;
    for (auto &arg : fn->args()) args.push_back(&arg);
    auto *call = b.CreateCall(worker, args);
    call->setCallingConv(fn->getCallingConv());
    call->setTailCallKind(CallInst::TCK_MustTail);
    if (fn->getReturnType()->isVoidTy()) b.CreateRetVoid();
    else b.CreateRet(call);
    leaf->removePredecessor(&worker->getEntryBlock());
    workerBranch->eraseFromParent();
    UncondBrInst::Create(slow, &worker->getEntryBlock());
    simplify(*worker, true);
  }
}
// One internal convention for direct calls, function values and tail calls:
// ([aggregate output,] environment, source arguments, invocation, token, mode)
// -> native scalar value or void. Explicit result storage avoids automatic sret.
// Rare failures unwind to a checked runtime boundary, not through result bits.
// Mode-specific workers share the same signature. Stored function values keep
// their checked code identity; an internal resolver selects the generic body.
// Moving a value to a task cannot retain its producer's execution mode.
inline void specializeInternalCallModes(
    llvm::Module &module, llvm::ArrayRef<llvm::Function *> bodies,
    void (*simplify)(llvm::Function &, bool)) {
  using namespace llvm;
  std::unordered_map<llvm::Function *, std::array<llvm::Function *, 3>> workers;
  for (auto *body : bodies) {
    std::array<llvm::Function *, 3> versions{};
    for (unsigned mode = 0; mode != 3; ++mode) {
      ValueToValueMapTy values;
      values[body] = body;
      auto *worker = CloneFunction(body, values);
      worker->setName(body->getName() + (mode == 0 ? ".clear" : mode == 1 ? ".token" : ".observed"));
      worker->setLinkage(GlobalValue::InternalLinkage);
      worker->getArg(worker->arg_size() - 1)->replaceAllUsesWith(
          ConstantInt::get(Type::getInt64Ty(module.getContext()), mode));
      versions[mode] = worker;
    }
    workers.emplace(body, versions);
  }
  for (auto *body : bodies)
    for (unsigned mode = 0; mode != 3; ++mode) {
      auto *worker = workers.at(body)[mode];
      SmallVector<CallInst *, 8> checkpoints;
      for (auto &block : *worker)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction)) {
            if (auto found = workers.find(call->getCalledFunction()); found != workers.end())
              call->setCalledFunction(found->second[mode]);
            if (mode != 2 && call->getMetadata("kalandra.checkpoint"))
              checkpoints.push_back(call);
          }
      for (auto *call : checkpoints) {
        IRBuilder<> b(call);
        Value *status = b.getInt64(0);
        if (mode == 1) {
          auto *cancelled = b.CreateLoad(b.getInt8Ty(), worker->getArg(worker->arg_size() - 2));
          cancelled->setAtomic(AtomicOrdering::Acquire);
          cancelled->setAlignment(Align(1));
          status = b.CreateSelect(b.CreateICmpEQ(cancelled, b.getInt8(0)), b.getInt64(0),
                                   b.getInt64(-1));
        }
        call->replaceAllUsesWith(status);
        call->eraseFromParent();
      }
      simplify(*worker, true);
    }
  for (auto *body : bodies) {
    body->deleteBody();
    body->setLinkage(GlobalValue::InternalLinkage);
    body->addFnAttr(Attribute::AlwaysInline);
    IRBuilder<> b(BasicBlock::Create(module.getContext(), "dispatch", body));
    auto *observed = BasicBlock::Create(module.getContext(), "observed", body);
    auto *dispatch = b.CreateSwitch(body->getArg(body->arg_size() - 1), observed, 2);
    for (unsigned mode = 0; mode != 3; ++mode) {
      auto *block = mode == 2 ? observed : BasicBlock::Create(module.getContext(),
                                                             mode == 0 ? "clear" : "token", body);
      if (mode != 2)
        dispatch->addCase(b.getInt64(mode), block);
      b.SetInsertPoint(block);
      SmallVector<Value *, 12> args;
      for (auto &arg : body->args())
        args.push_back(&arg);
      auto *call = b.CreateCall(workers.at(body)[mode], args);
      call->setCallingConv(body->getCallingConv());
      call->setTailCallKind(CallInst::TCK_MustTail);
      if (body->getReturnType()->isVoidTy())
        b.CreateRetVoid();
      else
        b.CreateRet(call);
    }
  }
}
} // namespace kalandra
