#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// A synchronous copy cannot migrate. Keep active source nodes on its executing
// thread's stack, rejecting malformed cycles without a logical depth limit.
// Keys include the destination invocation and copier type; shared DAG children
// remain valid after the preceding branch returns. No source value captures it.
class TaskTraversal {
  llvm::Function &function_;
  llvm::Value *head_, *previous_, *active_;

public:
  TaskTraversal(llvm::IRBuilder<> &builder, llvm::Function &function,
                 llvm::Value *sourceNode, llvm::BasicBlock *failed)
      : function_(function) {
    using namespace llvm;
    auto &module = *function.getParent();
    auto &context = module.getContext();
    auto *pointer = builder.getPtrTy();
    auto *type = StructType::get(pointer, pointer, pointer, pointer);
    auto *head = module.getGlobalVariable("task.traversal.head", true);
    if (!head) {
      head = new GlobalVariable(module, pointer, false, GlobalValue::InternalLinkage,
          ConstantPointerNull::get(pointer), "task.traversal.head");
      head->setThreadLocalMode(GlobalVariable::GeneralDynamicTLSModel);
    }
    IRBuilder<> entry(&*function.getEntryBlock().getFirstInsertionPt());
    auto *frame = entry.CreateAlloca(type, nullptr, "traversal.frame");
    previous_ = entry.CreateAlloca(pointer, nullptr, "traversal.previous");
    active_ = entry.CreateAlloca(entry.getInt1Ty(), nullptr, "traversal.active");
    entry.CreateStore(ConstantPointerNull::get(pointer), previous_);
    entry.CreateStore(entry.getFalse(), active_);
    head_ = entry.CreateThreadLocalAddress(head);
    auto *previous = builder.CreateLoad(pointer, head_);
    auto *start = builder.GetInsertBlock();
    auto *search = BasicBlock::Create(context, "traversal.search", &function);
    auto *probe = BasicBlock::Create(context, "traversal.probe", &function);
    auto *next = BasicBlock::Create(context, "traversal.next", &function);
    auto *cycle = BasicBlock::Create(context, "traversal.cycle", &function);
    auto *push = BasicBlock::Create(context, "traversal.push", &function);
    auto *ready = BasicBlock::Create(context, "traversal.ready", &function);
    builder.CreateBr(search);
    builder.SetInsertPoint(search);
    auto *current = builder.CreatePHI(pointer, 2);
    current->addIncoming(previous, start);
    builder.CreateCondBr(builder.CreateIsNull(current), push, probe);
    builder.SetInsertPoint(probe);
    auto load = [&](unsigned field) {
      return builder.CreateLoad(pointer, builder.CreateStructGEP(type, current, field));
    };
    auto *same = builder.CreateAnd(builder.CreateICmpEQ(load(1), function.getArg(0)),
        builder.CreateAnd(builder.CreateICmpEQ(load(2), sourceNode),
                          builder.CreateICmpEQ(load(3), &function)));
    builder.CreateCondBr(same, cycle, next);
    builder.SetInsertPoint(next);
    auto *parent = load(0);
    builder.CreateBr(search);
    current->addIncoming(parent, next);
    builder.SetInsertPoint(cycle);
    builder.CreateCall(module.getOrInsertFunction("kalandra_fault_v1", builder.getVoidTy(),
        pointer, builder.getInt64Ty(), builder.getInt64Ty(), builder.getInt64Ty()),
        {function.getArg(0), builder.getInt64(3), builder.getInt64(0), builder.getInt64(0)});
    builder.CreateBr(failed);
    builder.SetInsertPoint(push);
    builder.CreateStore(previous, previous_);
    builder.CreateStore(previous, builder.CreateStructGEP(type, frame, 0));
    builder.CreateStore(function.getArg(0), builder.CreateStructGEP(type, frame, 1));
    builder.CreateStore(sourceNode, builder.CreateStructGEP(type, frame, 2));
    builder.CreateStore(&function, builder.CreateStructGEP(type, frame, 3));
    builder.CreateStore(frame, head_);
    builder.CreateStore(builder.getTrue(), active_);
    builder.CreateBr(ready);
    builder.SetInsertPoint(ready);
  }
  void finish() {
    using namespace llvm;
    SmallVector<ReturnInst *, 8> returns;
    for (auto &block : function_)
      if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator())) returns.push_back(ret);
    for (auto *ret : returns) {
      auto *before = ret->getParent();
      auto *value = ret->getReturnValue();
      ret->eraseFromParent();
      auto *pop = BasicBlock::Create(function_.getContext(), "traversal.pop", &function_);
      auto *done = BasicBlock::Create(function_.getContext(), "traversal.return", &function_);
      IRBuilder<> builder(before);
      builder.CreateCondBr(builder.CreateLoad(builder.getInt1Ty(), active_), pop, done);
      builder.SetInsertPoint(pop);
      builder.CreateStore(builder.CreateLoad(builder.getPtrTy(), previous_), head_);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      if (value) builder.CreateRet(value);
      else builder.CreateRetVoid();
    }
  }
};
} // namespace kalandra
