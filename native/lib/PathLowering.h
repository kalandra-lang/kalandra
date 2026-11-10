#pragma once
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
namespace kalandra {
inline llvm::Type *pathStringType(const TaskValueTypes &types) {
  ValueType type;
  type.builtin = BuiltinValue::String;
  return types.scalar(type);
}
inline llvm::Value *emitPathCall(llvm::IRBuilder<> &b, llvm::Module &module,
                                 const TaskValueTypes &types, PathAction action, llvm::Value *input,
                                 llvm::Value *scratch, llvm::Value *ctx) {
  auto callee = module.getOrInsertFunction("kalandra_path_v1", b.getInt64Ty(), b.getPtrTy(),
                                           b.getPtrTy(), b.getInt32Ty(), b.getPtrTy());
  auto *flags = b.CreateCall(callee, {ctx, scratch, b.getInt32(unsigned(action)), input});
  return action == PathAction::IsAbsolute ? b.CreateICmpNE(flags, b.getInt64(0))
                                          : b.CreateLoad(pathStringType(types), scratch);
}
} // namespace kalandra
