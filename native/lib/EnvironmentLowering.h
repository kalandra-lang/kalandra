#pragma once
#include "../runtime/Environment.h"
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
inline void emitEnvironmentLookup(llvm::IRBuilder<> &builder, llvm::Module &module,
                                  const TaskValueTypes &types, const ValueType &result,
                                  llvm::StructType *resultType, llvm::Value *name,
                                  llvm::Value *output, llvm::Value *scratch, llvm::Value *context,
                                  llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  auto *function = builder.GetInsertBlock()->getParent();
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, function);
  };
  builder.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto lookup =
      module.getOrInsertFunction("kalandra_environment_get_v1", builder.getInt32Ty(),
                                 builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy());
  auto *status = builder.CreateCall(lookup, {context, scratch, name});
  auto ready = module.getOrInsertFunction("kalandra_invocation_ready_v1", builder.getInt1Ty(),
                                          builder.getPtrTy());
  auto *dispatch = block("env.ready");
  builder.CreateCondBr(builder.CreateCall(ready, {context}), dispatch, abort);
  builder.SetInsertPoint(dispatch);
  auto *corrupt = block("env.corrupt");
  auto *select = builder.CreateSwitch(status, corrupt, 4);

  using runtime::EnvironmentResult;
  auto *valueType = types.records()[*result.record];
  for (const auto resultKind : {EnvironmentResult::Missing, EnvironmentResult::Value}) {
    const bool present = resultKind == EnvironmentResult::Value;
    auto *success = block(present ? "env.value" : "env.missing");
    select->addCase(builder.getInt32(unsigned(resultKind)), success);
    builder.SetInsertPoint(success);
    auto *value = builder.CreateStructGEP(resultType, output, 1);
    builder.CreateStore(builder.getInt64(present ? 2 : 1),
                        builder.CreateStructGEP(valueType, value, 0));
    if (present)
      builder.CreateStore(builder.CreateLoad(valueType->getElementType(1), scratch),
                          builder.CreateStructGEP(valueType, value, 1));
    builder.CreateBr(next);
  }
  auto *errorType = types.records()[*result.errorRecord];
  for (const auto resultKind :
       {EnvironmentResult::InvalidName, EnvironmentResult::InvalidEncoding}) {
    const bool invalidName = resultKind == EnvironmentResult::InvalidName;
    auto *failure = block(invalidName ? "env.invalid.name" : "env.invalid.encoding");
    select->addCase(builder.getInt32(unsigned(resultKind)), failure);
    builder.SetInsertPoint(failure);
    auto *error = builder.CreateStructGEP(resultType, output, 2);
    builder.CreateStore(builder.getInt64(invalidName ? 1 : 2),
                        builder.CreateStructGEP(errorType, error, 0));
    builder.CreateBr(next);
  }
  builder.SetInsertPoint(corrupt);
  auto fault =
      module.getOrInsertFunction("kalandra_fault_v1", builder.getVoidTy(), builder.getPtrTy(),
                                 builder.getInt64Ty(), builder.getInt64Ty(), builder.getInt64Ty());
  builder.CreateCall(fault,
                     {context, builder.getInt64(3), builder.getInt64(0), builder.getInt64(0)});
  builder.CreateBr(abort);
}
} // namespace kalandra
