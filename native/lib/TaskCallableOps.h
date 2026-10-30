#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <stdexcept>
namespace kalandra {
// Checked local transport for compiler-known recursive callable entries.
// Signature/target admission belongs to the caller; this checks the pair and
// retains a code-bound immutable capture graph without modifying its fields.
class TaskCallableOps {
public:
  explicit TaskCallableOps(llvm::Module &module) : module_(module) {}
  llvm::Value *retain(llvm::IRBuilder<> &builder, llvm::Value *destination, llvm::Value *source,
                      llvm::Value *closure, llvm::Value *expectedCode, llvm::StructType *captures,
                      llvm::BasicBlock *failed) {
    auto *pair = closure ? llvm::dyn_cast<llvm::StructType>(closure->getType()) : nullptr;
    if (!pair || pair->getNumElements() != 2 || !pair->getElementType(0)->isPointerTy() ||
        !pair->getElementType(1)->isPointerTy() || !destination || !source || !expectedCode ||
        !destination->getType()->isPointerTy() || !source->getType()->isPointerTy() ||
        !expectedCode->getType()->isPointerTy() || !captures || !captures->isSized() || !failed ||
        !builder.GetInsertBlock() || failed->getParent() != builder.GetInsertBlock()->getParent())
      throw std::invalid_argument(
          "task callable transport requires a checked pair and capture layout");
    poll(builder, destination, failed);
    auto *invalid =
        llvm::BasicBlock::Create(builder.getContext(), "callable.invalid", failed->getParent());
    ready(builder,
          builder.CreateAnd(
              builder.CreateIsNotNull(expectedCode),
              builder.CreateICmpEQ(builder.CreateExtractValue(closure, {0}), expectedCode)),
          invalid, "callable.known");
    auto *environment = builder.CreateExtractValue(closure, {1});
    if (captures->getNumElements()) {
      const auto &layout = module_.getDataLayout();
      auto bridge = module_.getOrInsertFunction(
          "kalandra_recursive_environment_transfer_v1", builder.getPtrTy(), builder.getPtrTy(),
          builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty(), builder.getInt64Ty(),
          builder.getPtrTy());
      auto *retained = builder.CreateCall(
          bridge, {destination, source, environment,
                   builder.getInt64(layout.getTypeAllocSize(captures).getFixedValue()),
                   builder.getInt64(layout.getABITypeAlign(captures).value()), expectedCode});
      ready(builder, builder.CreateIsNotNull(retained), failed, "callable.retained");
    } else {
      ready(builder, builder.CreateIsNull(environment), invalid, "callable.capture.free");
    }
    poll(builder, destination, failed);
    auto *done = builder.GetInsertBlock();
    builder.SetInsertPoint(invalid);
    auto fault = module_.getOrInsertFunction("kalandra_fault_v1", builder.getVoidTy(),
                                             builder.getPtrTy(), builder.getInt64Ty(),
                                             builder.getInt64Ty(), builder.getInt64Ty());
    builder.CreateCall(
        fault, {destination, builder.getInt64(3), builder.getInt64(0), builder.getInt64(0)});
    builder.CreateBr(failed);
    builder.SetInsertPoint(done);
    return closure;
  }

private:
  static void ready(llvm::IRBuilder<> &builder, llvm::Value *condition, llvm::BasicBlock *failed,
                    const char *name) {
    auto *next = llvm::BasicBlock::Create(builder.getContext(), name, failed->getParent());
    builder.CreateCondBr(condition, next, failed);
    builder.SetInsertPoint(next);
  }
  void poll(llvm::IRBuilder<> &builder, llvm::Value *context, llvm::BasicBlock *failed) {
    auto readyCall = module_.getOrInsertFunction("kalandra_invocation_ready_v1",
                                                 builder.getInt1Ty(), builder.getPtrTy());
    ready(builder, builder.CreateCall(readyCall, {context}), failed, "callable.ready");
  }
  llvm::Module &module_;
};
} // namespace kalandra
