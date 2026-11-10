#pragma once
#include "../lib/PathLowering.h"
#include "llvm/IR/Verifier.h"
namespace kalandra {
inline llvm::Function *emitPathHostBridge(const TypedModule &input, llvm::Module &module,
                                          const PathOperation &operation, const ValueType &,
                                          llvm::StringRef name) {
  auto made = TaskValueTypes::create(input, module.getContext());
  if (!made)
    throw std::invalid_argument(made.error().message);
  auto &types = *made;
  auto *pointer = llvm::PointerType::getUnqual(module.getContext());
  auto *function =
      llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()),
                                                     {pointer, pointer, pointer}, false),
                             llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", function));
  auto *scratch = b.CreateAlloca(pathStringType(types));
  auto *value = emitPathCall(b, module, types, operation.action, function->getArg(1), scratch,
                             function->getArg(0));
  auto ready = module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), pointer);
  auto *success = llvm::BasicBlock::Create(module.getContext(), "success", function);
  auto *abort = llvm::BasicBlock::Create(module.getContext(), "faulted", function);
  b.CreateCondBr(b.CreateCall(ready, {function->getArg(0)}), success, abort);
  b.SetInsertPoint(success);
  b.CreateStore(value, function->getArg(2));
  b.CreateRetVoid();
  b.SetInsertPoint(abort);
  b.CreateRetVoid();
  if (llvm::verifyFunction(*function))
    throw std::invalid_argument("path host bridge failed LLVM verification");
  return function;
}
} // namespace kalandra
