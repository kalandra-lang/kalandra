#pragma once
#include "../runtime/Runtime.h"
#include "NativeStack.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace kalandra {
// Prove the original (unchecked) scalar result is returned unchanged. Only
// PHIs and unconditional branches may follow the call, including shared joins.
inline bool scalarTailReturn(llvm::CallInst &call) {
  using namespace llvm;
  auto &function = *call.getFunction();
  const auto scalar = [](Type *type) { return type->isIntegerTy() || type->isFloatingPointTy(); };
  if (call.getCalledFunction() != &function || !scalar(function.getReturnType()))
    return false;
  for (auto &argument : function.args())
    if (!scalar(argument.getType()))
      return false;
  for (auto &block : function)
    for (auto &instruction : block)
      if (isa<AllocaInst>(instruction))
        return false;
  SmallPtrSet<BasicBlock *, 16> visited;
  SmallPtrSet<Instruction *, 16> allowed;
  std::vector<Value *> values{&call};
  Value *value = &call;
  auto *block = call.getParent();
  auto at = std::next(call.getIterator());
  visited.insert(block);
  while (true) {
    if (at == block->end())
      return false;
    auto *instruction = &*at++;
    if (auto *ret = dyn_cast<ReturnInst>(instruction)) {
      if (ret->getReturnValue() != value)
        return false;
      allowed.insert(ret);
      break;
    }
    auto *branch = dyn_cast<UncondBrInst>(instruction);
    if (!branch)
      return false;
    auto *previous = block;
    block = branch->getSuccessor(0);
    if (!visited.insert(block).second)
      return false;
    for (auto &phi : block->phis()) {
      allowed.insert(&phi);
      if (phi.getIncomingValueForBlock(previous) == value) {
        value = &phi;
        values.push_back(value);
      }
    }
    at = block->getFirstNonPHIIt();
  }
  for (auto *forwarded : values)
    for (auto *user : forwarded->users())
      if (!allowed.contains(dyn_cast<Instruction>(user)))
        return false;
  return true;
}

// Reuse a physical frame, never a logical call slot. Each tail iteration runs
// logical admission; observed/token returns unwind every slot with the original
// post-call poll between them. Frame-local storage never qualifies.
inline void lowerCheckedTailCalls(llvm::Function &function,
                                  const std::vector<llvm::CallInst *> &tails,
                                  llvm::FunctionCallee enter, llvm::FunctionCallee leave,
                                  llvm::FunctionCallee poll, bool pureScalarBody) {
  using namespace llvm;
  if (tails.empty())
    return;
  auto &context = function.getContext();
  auto *invocation = function.getArg(function.arg_size() - 1);
  auto *i64 = Type::getInt64Ty(context);
  auto *zero = ConstantInt::get(i64, 0);
  auto *one = ConstantInt::get(i64, 1);
  auto *null = Constant::getNullValue(function.getReturnType());
  // Keep the full observed admission out of this loop. Inlining its TLS-address
  // intrinsic lets LICM speculate a Darwin TLS lookup even on the private path.
  auto observedEnter = function.getParent()->getOrInsertFunction("kalandra_tail_enter_v1",
                                                                 enter.getFunctionType());
  auto *entry = &function.getEntryBlock();
  // Checked ABI has one entry gate; only admitted paths reach this loop.
  auto *gate = cast<CondBrInst>(entry->getTerminator());
  auto *start = gate->getSuccessor(0);
  // Only a proven scalar body with no memory, callbacks or non-tail calls may
  // keep its logical depth in SSA. Invocation ownership is exclusive and no
  // observer is bound. The only asynchronous input is an optional atomic token,
  // loaded on every admission. Other state changes are terminal local faults.
  IRBuilder<> prelude(gate);
  auto field = [&](IRBuilder<> &b, std::size_t offset) {
    return b.CreateConstGEP1_64(b.getInt8Ty(), invocation, offset);
  };
  auto *baseDepth = prelude.CreateLoad(i64, field(prelude, runtime::InvocationABI::depth));
  Value *privateDepth = prelude.getFalse();
  Value *hasToken = prelude.getFalse();
  Value *token = nullptr;
  if (pureScalarBody) {
    auto *mode = prelude.CreateLoad(prelude.getInt8Ty(),
                                   field(prelude, runtime::InvocationABI::pollMode));
    privateDepth = prelude.CreateICmpULT(mode, prelude.getInt8(2), "tail.private.depth");
    hasToken = prelude.CreateICmpEQ(mode, prelude.getInt8(1));
    token = prelude.CreateLoad(PointerType::getUnqual(context),
                               field(prelude, runtime::InvocationABI::cancellation));
  }
  auto *header = BasicBlock::Create(context, "tail.header", &function, start);
  IRBuilder<> builder(header);
  std::vector<PHINode *> parameters;
  for (unsigned i = 0; i + 1 < function.arg_size(); ++i) {
    auto *argument = function.getArg(i);
    auto *phi = builder.CreatePHI(argument->getType(), tails.size() + 1, "tail.argument");
    argument->replaceAllUsesWith(phi);
    phi->addIncoming(argument, entry);
    parameters.push_back(phi);
  }
  auto *frames = builder.CreatePHI(i64, tails.size() + 1, "tail.frames");
  frames->addIncoming(zero, entry);
  builder.CreateBr(start);
  gate->setSuccessor(0, header);
  // All admitted returns currently have one balancing leave immediately before
  // ret. The denied entry acquired no slot and must remain outside the unwind.
  std::vector<ReturnInst *> returns;
  for (auto &block : function)
    if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
      if (auto *call = dyn_cast_or_null<CallInst>(ret->getPrevNode());
          call && call->getCalledOperand() == leave.getCallee())
        returns.push_back(ret);
  auto *unwind = BasicBlock::Create(context, "tail.unwind", &function);
  auto *ordinary = BasicBlock::Create(context, "tail.return.one", &function);
  auto *batch = BasicBlock::Create(context, "tail.return.clear", &function);
  auto *next = BasicBlock::Create(context, "tail.return.poll", &function);
  auto *done = BasicBlock::Create(context, "tail.done", &function);
  builder.SetInsertPoint(unwind);
  auto *remaining = builder.CreatePHI(i64, returns.size() + 1, "tail.remaining");
  auto *result = builder.CreatePHI(function.getReturnType(), returns.size() + 1, "tail.result");
  // With no token, observer, fault or settlement, all return polls are proven
  // clear. Balance the same logical slots in one update. Observed and token
  // modes retain every original leave/poll in its original logical order.
  auto address = [&](std::size_t offset) {
    return builder.CreateConstGEP1_64(builder.getInt8Ty(), invocation, offset);
  };
  auto *mode = builder.CreateLoad(builder.getInt8Ty(), address(runtime::InvocationABI::pollMode));
  auto *depthAddress = address(runtime::InvocationABI::depth);
  auto *depth = builder.CreateLoad(i64, depthAddress);
  auto *slots = builder.CreateAdd(remaining, one);
  auto *clear = builder.CreateAnd(builder.CreateICmpEQ(mode, builder.getInt8(0)),
                                 builder.CreateICmpUGE(depth, slots));
  builder.CreateCondBr(clear, batch, ordinary);
  builder.SetInsertPoint(batch);
  auto *restored = builder.CreateSub(depth, slots);
#if defined(__APPLE__)
  auto *leaseAddress = address(runtime::InvocationABI::leaseDepth);
  auto *lease = builder.CreateLoad(i64, leaseAddress);
  auto *released = builder.CreateAnd(builder.CreateICmpUGT(lease, restored),
                                    builder.CreateICmpULE(lease, depth));
  builder.CreateStore(builder.CreateSelect(released, zero, lease), leaseAddress);
#endif
  builder.CreateStore(restored, depthAddress);
  builder.CreateBr(done);
  builder.SetInsertPoint(ordinary);
  builder.CreateCall(leave, {invocation});
  builder.CreateCondBr(builder.CreateICmpEQ(remaining, zero), done, next);
  builder.SetInsertPoint(next);
  auto *status = builder.CreateCall(poll, {invocation});
  auto *value = builder.CreateSelect(builder.CreateICmpEQ(status, zero), result, null);
  auto *count = builder.CreateSub(remaining, one);
  builder.CreateBr(unwind);
  remaining->addIncoming(count, next);
  result->addIncoming(value, next);
  builder.SetInsertPoint(done);
  auto *finished = builder.CreatePHI(function.getReturnType(), 2);
  finished->addIncoming(result, batch);
  finished->addIncoming(result, ordinary);
  builder.CreateRet(finished);
  for (auto *ret : returns) {
    auto *block = ret->getParent();
    remaining->addIncoming(frames, block);
    result->addIncoming(ret->getReturnValue(), block);
    ret->getPrevNode()->eraseFromParent();
    builder.SetInsertPoint(ret);
    if (pureScalarBody) {
      auto *address = field(builder, runtime::InvocationABI::depth);
      auto *published = builder.CreateLoad(i64, address);
      builder.CreateStore(builder.CreateSelect(privateDepth, builder.CreateAdd(baseDepth, frames),
                                               published), address);
    }
    builder.CreateBr(unwind);
    ret->eraseFromParent();
  }
  for (auto *call : tails) {
    auto *block = call->getParent();
    auto *continuation = block->splitBasicBlock(std::next(call->getIterator()), "tail.denied");
    block->getTerminator()->eraseFromParent();
    auto *accepted = BasicBlock::Create(context, "tail.accepted", &function, continuation);
    builder.SetInsertPoint(accepted);
    for (unsigned i = 0; i < parameters.size(); ++i)
      parameters[i]->addIncoming(call->getArgOperand(i), accepted);
    auto *depth = builder.CreateAdd(frames, one);
    frames->addIncoming(depth, accepted);
    builder.CreateBr(header);
    builder.SetInsertPoint(block);
    if (pureScalarBody) {
      auto *ordinary = BasicBlock::Create(context, "tail.admit.observed", &function, continuation);
      auto *privateEntry = BasicBlock::Create(context, "tail.admit.private", &function, continuation);
      auto *tokenCheck = BasicBlock::Create(context, "tail.admit.token", &function, continuation);
      auto *depthCheck = BasicBlock::Create(context, "tail.admit.depth", &function, continuation);
      auto *exhausted = BasicBlock::Create(context, "tail.exhausted", &function, continuation);
      builder.CreateCondBr(privateDepth, privateEntry, ordinary);
      builder.SetInsertPoint(privateEntry);
      builder.CreateCondBr(hasToken, tokenCheck, depthCheck);
      builder.SetInsertPoint(tokenCheck);
      auto *cancelled = builder.CreateLoad(builder.getInt8Ty(), token);
      cancelled->setAtomic(AtomicOrdering::Acquire);
      cancelled->setAlignment(Align(1));
      // Match enter's poll-before-depth ordering. Denied admission acquires no
      // slot, and follows the original post-call poll and checked unwind.
      builder.CreateCondBr(builder.CreateICmpNE(cancelled, builder.getInt8(0)),
                           continuation, depthCheck);
      builder.SetInsertPoint(depthCheck);
      // No allocas/callbacks: reusing the admitted physical frame cannot consume
      // more stack. Logical depth is still checked on every source call.
      auto *logicalDepth = builder.CreateAdd(baseDepth, frames);
      builder.CreateCondBr(builder.CreateICmpULT(logicalDepth,
                            builder.getInt64(runtime::Invocation::maxCallDepth)), accepted, exhausted);
      builder.SetInsertPoint(exhausted);
      auto fault = function.getParent()->getOrInsertFunction("kalandra_fault_v1",
          builder.getVoidTy(), invocation->getType(), i64, i64, i64);
      builder.CreateCall(fault, {invocation, builder.getInt64(6), zero, zero});
      builder.CreateBr(continuation);
      builder.SetInsertPoint(ordinary);
    }
    auto *admitted = builder.CreateCall(observedEnter, {invocation, zero, zero});
    builder.CreateCondBr(admitted, accepted, continuation);
    call->replaceAllUsesWith(null);
    call->eraseFromParent();
  }
  // The same closed scalar proof that permits SSA admission depth permits
  // batched return bookkeeping when only a token is bound. No observer,
  // callback or fault can run inside this loop: preserve every acquire poll,
  // keep cancellation sticky in the result, and publish balanced depth/lease
  // once at its exit. Faulted/observed modes retain the original unwind.
  // This optional expansion never changes source acceptance near the budget.
  if (pureScalarBody &&
      !nativeStackBudgetError(function, function.getParent()->getDataLayout(), false, 1024)) {
    auto *check = BasicBlock::Create(context, "tail.return.token.check", &function, ordinary);
    auto *loop = BasicBlock::Create(context, "tail.return.token", &function, batch);
    auto *pollToken = BasicBlock::Create(context, "tail.return.token.poll", &function, batch);
    cast<CondBrInst>(unwind->getTerminator())->setSuccessor(1, check);
    builder.SetInsertPoint(check);
    auto *privateToken = builder.CreateAnd(builder.CreateICmpEQ(mode, builder.getInt8(1)),
                                           builder.CreateICmpUGE(depth, slots));
    builder.CreateCondBr(privateToken, loop, ordinary);
    builder.SetInsertPoint(loop);
    auto *tokenRemaining = builder.CreatePHI(i64, 2, "tail.token.remaining");
    auto *tokenResult = builder.CreatePHI(function.getReturnType(), 2, "tail.token.result");
    tokenRemaining->addIncoming(remaining, check);
    tokenResult->addIncoming(result, check);
    builder.CreateCondBr(builder.CreateICmpEQ(tokenRemaining, zero), batch, pollToken);
    builder.SetInsertPoint(pollToken);
    auto *cancelled = builder.CreateLoad(builder.getInt8Ty(), token);
    cancelled->setAtomic(AtomicOrdering::Acquire);
    cancelled->setAlignment(Align(1));
    auto *nextResult = builder.CreateSelect(builder.CreateICmpNE(cancelled, builder.getInt8(0)),
                                            null, tokenResult);
    auto *nextRemaining = builder.CreateSub(tokenRemaining, one);
    builder.CreateBr(loop);
    tokenRemaining->addIncoming(nextRemaining, pollToken);
    tokenResult->addIncoming(nextResult, pollToken);
    builder.SetInsertPoint(&*batch->getFirstInsertionPt());
    auto *batchResult = builder.CreatePHI(function.getReturnType(), 2, "tail.batch.result");
    batchResult->addIncoming(result, unwind);
    batchResult->addIncoming(tokenResult, loop);
    finished->setIncomingValueForBlock(batch, batchResult);
  }
}
} // namespace kalandra
