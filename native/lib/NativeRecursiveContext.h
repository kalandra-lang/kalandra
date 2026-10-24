#pragma once
#include "../runtime/Runtime.h"
#include "NativeStack.h"
#include "NativeRecursiveDepth.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <array>
#include <unordered_map>

namespace kalandra {
// Only a closed scalar call graph can establish these invariants: no observer,
// callbacks, allocation, token rebinding or settlement; a live outer stack lease.
// Faults remain terminal and must still be checked before cancellation tokens.
inline void specializeRecursiveContexts(llvm::Module &module,
                                        const llvm::SmallPtrSetImpl<llvm::Function *> &eligible,
                                        const llvm::SmallPtrSetImpl<llvm::Function *> &pureTails,
                                        void (*simplify)(llvm::Function &)) {
  using namespace llvm;
  using Layout = runtime::InvocationABI;
  auto &context = module.getContext();
  auto *ptr = PointerType::getUnqual(context);
  auto *i64 = Type::getInt64Ty(context);
  auto field = [&](IRBuilder<> &b, Value *invocation, std::size_t offset) {
    return b.CreateConstGEP1_64(b.getInt8Ty(), invocation, offset);
  };
  auto helper = [&](const Twine &name, Type *result, ArrayRef<Type *> parameters) {
    auto *fn = Function::Create(FunctionType::get(result, parameters, false),
                                GlobalValue::InternalLinkage, name, module);
    fn->addFnAttr(Attribute::AlwaysInline);
    fn->addFnAttr(Attribute::NoUnwind);
    BasicBlock::Create(context, "entry", fn);
    return fn;
  };
  Function *polls[2] = {}, *enters[2][2] = {};
  Function *leave = nullptr;
  auto defineHelpers = [&] {
    leave = helper("kalandra_recursive_leave_v1", Type::getVoidTy(context), {ptr});
    IRBuilder<> b(&leave->getEntryBlock());
    auto *address = field(b, leave->getArg(0), Layout::depth);
    b.CreateStore(b.CreateSub(b.CreateLoad(i64, address), b.getInt64(1)), address);
    b.CreateRetVoid();
    for (unsigned bound = 0; bound != 2; ++bound) {
      const auto suffix = bound ? "token" : "clear";
      auto *poll = helper(std::string("kalandra_recursive_poll_") + suffix, i64, {ptr, ptr});
      polls[bound] = poll;
      b.SetInsertPoint(&poll->getEntryBlock());
      auto *mode = b.CreateLoad(b.getInt8Ty(), field(b, poll->getArg(0), Layout::pollMode));
      auto *failed = BasicBlock::Create(context, "fault", poll);
      auto *clear = BasicBlock::Create(context, "clear", poll);
      b.CreateCondBr(b.CreateICmpEQ(mode, b.getInt8(2)), failed, clear);
      b.SetInsertPoint(failed);
      auto slow = module.getOrInsertFunction("kalandra_poll_slow_v1", i64, ptr);
      b.CreateRet(b.CreateCall(slow, {poll->getArg(0)}));
      b.SetInsertPoint(clear);
      if (bound) {
        auto *cancelled = b.CreateLoad(b.getInt8Ty(), poll->getArg(1));
        cancelled->setAtomic(AtomicOrdering::Acquire);
        cancelled->setAlignment(Align(1));
        b.CreateRet(
            b.CreateSelect(b.CreateICmpNE(cancelled, b.getInt8(0)), b.getInt64(-1), b.getInt64(0)));
      } else
        b.CreateRet(b.getInt64(0));
      for (unsigned physical = 0; physical != 2; ++physical) {
        auto *enter = helper(std::string(physical ? "kalandra_recursive_enter_"
                                                  : "kalandra_recursive_inline_enter_") +
                                 suffix,
                             b.getInt1Ty(), {ptr, ptr, i64, i64});
        enters[bound][physical] = enter;
        b.SetInsertPoint(&enter->getEntryBlock());
        auto *invocation = enter->getArg(0);
        auto *status = b.CreateCall(poll, {invocation, enter->getArg(1)});
        auto *check = BasicBlock::Create(context, "depth", enter);
        auto *denied = BasicBlock::Create(context, "denied", enter);
        auto *exhausted = BasicBlock::Create(context, "exhausted", enter);
        auto *accepted = BasicBlock::Create(context, "accepted", enter);
        b.CreateCondBr(b.CreateICmpEQ(status, b.getInt64(0)), check, denied);
        b.SetInsertPoint(check);
        auto *depth = b.CreateLoad(i64, field(b, invocation, Layout::depth));
#if defined(__APPLE__)
        if (physical) {
          auto *stack = BasicBlock::Create(context, "stack", enter);
          b.CreateCondBr(b.CreateICmpULT(depth, b.getInt64(runtime::Invocation::maxCallDepth)),
                         stack, exhausted);
          b.SetInsertPoint(stack);
          auto *low = b.CreateLoad(i64, field(b, invocation, Layout::stackLow));
          auto *high = b.CreateLoad(i64, field(b, invocation, Layout::stackHigh));
          auto *sp = b.CreatePtrToInt(b.CreateStackSave(), i64);
          b.CreateCondBr(b.CreateAnd(b.CreateICmpUGT(sp, low), b.CreateICmpULE(sp, high)), accepted,
                         exhausted);
        } else
#endif
          b.CreateCondBr(b.CreateICmpULT(depth, b.getInt64(runtime::Invocation::maxCallDepth)),
                         accepted, exhausted);
        b.SetInsertPoint(accepted);
        b.CreateStore(b.CreateAdd(depth, b.getInt64(1)), field(b, invocation, Layout::depth));
        b.CreateRet(b.getTrue());
        b.SetInsertPoint(exhausted);
        b.CreateCall(module.getFunction("kalandra_fault_v1"),
                     {invocation, b.getInt64(6), enter->getArg(2), enter->getArg(3)});
        b.CreateBr(denied);
        b.SetInsertPoint(denied);
        b.CreateRet(b.getFalse());
      }
    }
  };
  // Keep module order stable, independent of pointer/set iteration order.
  SmallVector<Function *, 8> functions;
  SmallVector<Function *, 16> publicFunctions;
  for (auto &fn : module)
    if (eligible.contains(&fn))
      functions.push_back(&fn);
  for (auto &fn : module)
    if (!fn.isDeclaration())
      if (auto *branch = dyn_cast<CondBrInst>(fn.getEntryBlock().getTerminator()))
        if (auto *call = dyn_cast<CallInst>(branch->getCondition());
            call && call->getCalledFunction() == module.getFunction("kalandra_call_enter_v1"))
          publicFunctions.push_back(&fn);
  std::unordered_map<Function *, std::array<Function *, 3>> versions;
  std::unordered_map<Function *, Function *> dispatchers;
  for (auto *function : functions) {
    DominatorTree dominators(*function);
    LoopInfo loops(dominators);
    unsigned size = 0;
    bool fixedFrame = true;
    for (auto &block : *function)
      for (auto &instruction : block) {
        ++size;
        fixedFrame &= !isa<AllocaInst>(instruction);
      }
    auto *gate = dyn_cast<CondBrInst>(function->getEntryBlock().getTerminator());
    auto *admission = gate ? dyn_cast<CallInst>(gate->getCondition()) : nullptr;
    if (!fixedFrame || !admission ||
        admission->getCalledFunction() != module.getFunction("kalandra_call_enter_v1") ||
        (!loops.empty() && !pureTails.contains(function)) || size > 750 ||
        nativeStackBudgetError(*function, module.getDataLayout(), false, 1024))
      continue;
    if (!leave)
      defineHelpers();
    Function *workers[2] = {};
    bool fits = true;
    for (unsigned bound = 0; bound != 2; ++bound) {
      SmallVector<Type *, 10> parameters(function->getFunctionType()->params());
      parameters.push_back(ptr);
      auto *worker = Function::Create(
          FunctionType::get(function->getReturnType(), parameters, false),
          GlobalValue::InternalLinkage,
          function->getName() + (bound ? ".recursive.token" : ".recursive.clear"), module);
      workers[bound] = worker;
      ValueToValueMapTy values;
      values[function] = function;
      for (auto &argument : function->args())
        values[&argument] = worker->getArg(argument.getArgNo());
      SmallVector<ReturnInst *, 8> returns;
      CloneFunctionInto(worker, function, values, CloneFunctionChangeType::LocalChangesOnly,
                        returns);
      worker->setLinkage(GlobalValue::InternalLinkage);
      worker->addFnAttr(Attribute::NoInline);
      auto *token = worker->getArg(worker->arg_size() - 1);
      token->setName("recursive.token");
      auto *invocation = worker->getArg(function->arg_size() - 1);
      if (pureTails.contains(function)) {
        // The initial mode/token are immutable during this closed scalar chain.
        // Do not replace later fault polls: a terminal fault still selects mode 2.
        for (auto &instruction : worker->getEntryBlock())
          if (auto *load = dyn_cast<LoadInst>(&instruction))
            if (auto *gep = dyn_cast<GetElementPtrInst>(load->getPointerOperand());
                gep && gep->getPointerOperand() == invocation && gep->getNumIndices() == 1)
              if (auto *offset = dyn_cast<ConstantInt>(gep->getOperand(1))) {
                if (offset->getZExtValue() == Layout::pollMode)
                  load->replaceAllUsesWith(ConstantInt::get(load->getType(), bound));
                else if (offset->getZExtValue() == Layout::cancellation)
                  load->replaceAllUsesWith(token);
              }
        SmallVector<StoreInst *, 4> leaseStores;
        for (auto &block : *worker)
          for (auto &instruction : block)
            if (auto *store = dyn_cast<StoreInst>(&instruction))
              if (auto *gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
                  gep && gep->getPointerOperand() == invocation && gep->getNumIndices() == 1)
                if (auto *offset = dyn_cast<ConstantInt>(gep->getOperand(1));
                    offset && offset->getZExtValue() == Layout::leaseDepth)
                  leaseStores.push_back(store);
        // These workers are strictly inside the outer lease, including batched
        // tail unwinding. Only the public owning frame can release that lease.
        for (auto *store : leaseStores)
          store->eraseFromParent();
      }
      auto *admittedBlock = cast<BasicBlock>(values[gate->getSuccessor(0)]);
      DominatorTree workerDominators(*worker);
      SmallVector<CallInst *, 16> calls;
      for (auto &block : *worker)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction))
            calls.push_back(call);
      for (auto *call : calls) {
        auto *callee = call->getCalledFunction();
        if (!callee)
          continue;
        SmallVector<Value *, 10> arguments(call->args());
        Function *target = nullptr;
        if (callee == function) {
          target = worker;
          arguments.push_back(token);
        } else if (callee->getName() == "kalandra_poll_v1") {
          target = polls[bound];
          arguments.push_back(token);
        } else if (callee->getName() == "kalandra_call_enter_v1") {
          // One physical frame check dominates every inlined logical admission.
          // The proven scalar frame has no allocas or variable stack storage.
          const bool physical = call == values[admission] ||
                                !workerDominators.dominates(admittedBlock, call->getParent());
          target = enters[bound][physical];
          arguments.insert(arguments.begin() + 1, token);
        } else if (callee->getName() == "kalandra_call_leave_v1")
          target = leave;
        if (target) {
          IRBuilder<> b(call);
          auto *replacement = b.CreateCall(target, arguments);
          replacement->setCallingConv(call->getCallingConv());
          call->replaceAllUsesWith(replacement);
          call->eraseFromParent();
        }
      }
      simplify(*worker);
      fits &= !nativeStackBudgetError(*worker, module.getDataLayout());
    }
    if (!fits) {
      for (auto *worker : workers)
        worker->dropAllReferences();
      for (auto *worker : workers)
        worker->eraseFromParent();
      continue;
    }
    ValueToValueMapTy observedValues;
    auto *observedBody = CloneFunction(function, observedValues);
    observedBody->setName(function->getName() + ".recursive.observed");
    observedBody->setLinkage(GlobalValue::InternalLinkage);
    observedBody->addFnAttr(Attribute::NoInline);
    versions.emplace(function, std::array<Function *, 3>{workers[0], workers[1], observedBody});
    // Only calls under an already admitted public frame can dispatch here.
    auto *dispatch = helper("kalandra_recursive_dispatch_" + function->getName(),
                            function->getReturnType(), function->getFunctionType()->params());
    IRBuilder<> b(&dispatch->getEntryBlock());
    SmallVector<Value *, 10> arguments;
    for (auto &argument : dispatch->args())
      arguments.push_back(&argument);
    auto *invocation = arguments.back();
    auto *mode = b.CreateLoad(b.getInt8Ty(), field(b, invocation, Layout::pollMode));
    auto *clear = BasicBlock::Create(context, "clear", dispatch);
    auto *tokenCheck = BasicBlock::Create(context, "token.check", dispatch);
    auto *bound = BasicBlock::Create(context, "token", dispatch);
    auto *observed = BasicBlock::Create(context, "observed", dispatch);
    b.CreateCondBr(b.CreateICmpEQ(mode, b.getInt8(0)), clear, tokenCheck);
    b.SetInsertPoint(clear);
    arguments.push_back(ConstantPointerNull::get(ptr));
    b.CreateRet(b.CreateCall(workers[0], arguments));
    b.SetInsertPoint(tokenCheck);
    b.CreateCondBr(b.CreateICmpEQ(mode, b.getInt8(1)), bound, observed);
    b.SetInsertPoint(bound);
    arguments.back() = b.CreateLoad(ptr, field(b, invocation, Layout::cancellation));
    b.CreateRet(b.CreateCall(workers[1], arguments));
    b.SetInsertPoint(observed);
    arguments.pop_back();
    b.CreateRet(b.CreateCall(observedBody, arguments));
    if (!nativeStackBudgetError(*dispatch, module.getDataLayout(), false, 16 * 1024 - 512))
      dispatchers.emplace(function, dispatch);
    else
      dispatch->eraseFromParent();
  }
  for (auto *function : publicFunctions) {
    DominatorTree dominators(*function);
    auto *gate = cast<CondBrInst>(function->getEntryBlock().getTerminator());
    SmallVector<CallInst *, 8> calls;
    for (auto &block : *function)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && dispatchers.contains(call->getCalledFunction()) &&
            dominators.dominates(gate->getSuccessor(0), &block))
          calls.push_back(call);
    // Charge the complete bounded dispatcher expansion, including its three
    // outgoing argument copies, before adding always-inline calls to a source
    // frame. Decline optimization without changing source acceptance.
    if (calls.size() >= 32 ||
        nativeStackBudgetError(*function, module.getDataLayout(), false, calls.size() * 512))
      continue;
    for (auto *call : calls)
      call->setCalledFunction(dispatchers.at(call->getCalledFunction()));
  }
  // Observers retain the original checked implementation and direct recursive
  // edges, avoiding a redundant dispatch at every observed logical call.
  for (auto &[original, workers] : versions)
    for (auto &block : *workers[2])
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction);
            call && versions.contains(call->getCalledFunction()))
          call->setCalledFunction(versions.at(call->getCalledFunction())[2]);
  for (auto &[original, workers] : versions)
    for (unsigned bound = 0; bound != 2; ++bound) {
      auto *worker = workers[bound];
      SmallVector<CallInst *, 8> calls;
      for (auto &block : *worker)
        for (auto &instruction : block)
          if (auto *call = dyn_cast<CallInst>(&instruction);
              call && versions.contains(call->getCalledFunction()))
            calls.push_back(call);
      // Additional token arguments must also fit the existing combined budget.
      if (nativeStackBudgetError(*worker, module.getDataLayout(), false, calls.size() * 8))
        continue;
      for (auto *call : calls) {
        auto *target = versions.at(call->getCalledFunction())[bound];
        SmallVector<Value *, 10> arguments(call->args());
        arguments.push_back(worker->getArg(worker->arg_size() - 1));
        IRBuilder<> b(call);
        auto *replacement = b.CreateCall(target, arguments);
        replacement->setCallingConv(call->getCallingConv());
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
      }
    }
  // Stable source order is part of reproducible module/package generation.
  // Token workers retain their acquire-based status path.
  for (auto *original : functions)
    if (auto found = versions.find(original); found != versions.end())
      specializeRecursiveDepth(*found->second[0], polls[0],
                               {leave, enters[0][0], enters[0][1]}, simplify);
}
} // namespace kalandra
