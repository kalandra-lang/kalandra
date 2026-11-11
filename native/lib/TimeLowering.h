#pragma once
#include "../runtime/Time.h"
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
namespace kalandra {
inline llvm::StructType *timeResultType(llvm::LLVMContext &context) {
  return llvm::StructType::get(context,
                               {llvm::Type::getInt64Ty(context), llvm::Type::getInt32Ty(context)});
}
// Returns the runtime word; for Sleep this is the checked native clock deadline.
// Only success enters `success`; application errors enter `next` immediately.
inline llvm::Value *emitTimeOperation(llvm::IRBuilder<> &b, llvm::Module &module,
                                      const TaskValueTypes &types, TimeAction action,
                                      const ValueType &result, llvm::StructType *packetType,
                                      llvm::Value *input, llvm::Value *packet, llvm::Value *scratch,
                                      llvm::Value *ctx, llvm::BasicBlock *success,
                                      llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  auto *fn = b.GetInsertBlock()->getParent();
  auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, fn);
  };
  b.CreateStore(llvm::Constant::getNullValue(packetType), packet);
  llvm::Value *word = action == TimeAction::Now ? static_cast<llvm::Value *>(b.getInt64(0))
                                                : b.CreateLoad(b.getInt64Ty(), input);
  auto callee = module.getOrInsertFunction("kalandra_time_v1", b.getVoidTy(), b.getPtrTy(),
                                           b.getPtrTy(), b.getInt32Ty(), b.getInt64Ty());
  b.CreateCall(callee, {ctx, scratch, b.getInt32(unsigned(action)), word});
  auto ready =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  auto *dispatch = block("time.ready");
  b.CreateCondBr(b.CreateCall(ready, {ctx}), dispatch, abort);
  b.SetInsertPoint(dispatch);
  auto *layout = timeResultType(module.getContext());
  auto *value = b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(layout, scratch, 0));
  auto *kind = b.CreateLoad(b.getInt32Ty(), b.CreateStructGEP(layout, scratch, 1));
  auto *corrupt = block("time.corrupt");
  auto *ok = block("time.success");
  auto *select = b.CreateSwitch(kind, corrupt, result.fallible ? 3 : 1);
  select->addCase(b.getInt32(0), ok);
  if (result.fallible) {
    for (unsigned tag : {1u, 2u}) {
      auto *error = block("time.error");
      select->addCase(b.getInt32(tag), error);
      b.SetInsertPoint(error);
      auto *payload = b.CreateStructGEP(packetType, packet, 2);
      b.CreateStore(b.getInt64(tag),
                    b.CreateStructGEP(types.records()[*result.errorRecord], payload, 0));
      b.CreateBr(next);
    }
  }
  b.SetInsertPoint(corrupt);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
  b.CreateCall(fault, {ctx, b.getInt64(3), b.getInt64(0), b.getInt64(0)});
  b.CreateBr(abort);
  b.SetInsertPoint(ok);
  auto *payload = b.CreateStructGEP(packetType, packet, 1);
  if (result.record)
    b.CreateStore(value, b.CreateStructGEP(types.records()[*result.record], payload, 0));
  else if (action == TimeAction::AsSeconds)
    b.CreateStore(b.CreateBitCast(value, b.getDoubleTy()), payload);
  else if (action != TimeAction::Sleep)
    b.CreateStore(value, payload);
  b.CreateBr(success);
  return value;
}
} // namespace kalandra
