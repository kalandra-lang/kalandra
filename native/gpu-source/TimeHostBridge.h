#pragma once
#include "../lib/TimeLowering.h"
#include "llvm/IR/Verifier.h"
namespace kalandra {
inline llvm::Function *emitTimeHostBridge(const TypedModule &input, llvm::Module &module,
                                          const TimeOperation &operation, const ValueType &result,
                                          llvm::StringRef name) {
  auto checked = TaskValueTypes::create(input, module.getContext());
  if (!checked)
    throw std::invalid_argument(checked.error().message);
  auto &types = *checked;
  auto *ptr = llvm::PointerType::getUnqual(module.getContext());
  auto *fn =
      llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getInt64Ty(module.getContext()),
                                                     {ptr, ptr, ptr, ptr, ptr}, false),
                             llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", fn));
  auto *packetType = types.value(result, "time.host.result");
  auto *packet = b.CreateAlloca(packetType);
  auto *scratch = b.CreateAlloca(timeResultType(module.getContext()));
  auto *complete = llvm::BasicBlock::Create(module.getContext(), "complete", fn);
  auto *abort = llvm::BasicBlock::Create(module.getContext(), "faulted", fn);
  emitTimeOperation(b, module, types, operation.action, result, packetType, fn->getArg(1), packet,
                    scratch, fn->getArg(0), complete, complete, abort);
  b.SetInsertPoint(complete);
  b.CreateStore(
      b.CreateLoad(packetType->getElementType(1), b.CreateStructGEP(packetType, packet, 1)),
      fn->getArg(2));
  if (result.fallible) {
    b.CreateStore(
        b.CreateLoad(packetType->getElementType(0), b.CreateStructGEP(packetType, packet, 0)),
        fn->getArg(3));
    b.CreateStore(
        b.CreateLoad(packetType->getElementType(2), b.CreateStructGEP(packetType, packet, 2)),
        fn->getArg(4));
  }
  b.CreateRet(b.CreateLoad(b.getInt64Ty(),
                           b.CreateStructGEP(timeResultType(module.getContext()), scratch, 0)));
  b.SetInsertPoint(abort);
  b.CreateRet(b.getInt64(0));
  if (llvm::verifyFunction(*fn))
    throw std::invalid_argument("time host bridge failed LLVM verification");
  return fn;
}
} // namespace kalandra
