#pragma once
#include "../lib/ParseLowering.h"
#include "llvm/IR/Verifier.h"
namespace kalandra {
// Synchronous pure host conversion: no I/O state, suspension or worker admission.
inline llvm::Function *emitParseHostBridge(const TypedModule &input, llvm::Module &module,
                                           const ParseOperation &operation, const ValueType &result,
                                           llvm::StringRef name) {
  auto checked = TaskValueTypes::create(input, module.getContext());
  if (!checked)
    throw std::invalid_argument(checked.error().message);
  auto &types = *checked;
  auto *pointer = llvm::PointerType::getUnqual(module.getContext());
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()),
                              {pointer, pointer, pointer, pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", function));
  auto *packetType = types.value(result, "parse.host.result");
  auto *packet = b.CreateAlloca(packetType);
  auto *scratch = b.CreateAlloca(parseResultType(module.getContext()));
  auto *complete = llvm::BasicBlock::Create(module.getContext(), "complete", function);
  auto *abort = llvm::BasicBlock::Create(module.getContext(), "faulted", function);
  emitParseOperation(b, module, input, types, operation, result, packetType, function->getArg(1),
                     packet, scratch, function->getArg(0), complete, abort);
  b.SetInsertPoint(complete);
  for (const auto &[field, destination] : {std::pair{1u, 2u}, std::pair{0u, 3u}, std::pair{2u, 4u}})
    b.CreateStore(b.CreateLoad(packetType->getElementType(field),
                               b.CreateStructGEP(packetType, packet, field)),
                  function->getArg(destination));
  b.CreateRetVoid();
  b.SetInsertPoint(abort);
  b.CreateRetVoid();
  if (llvm::verifyFunction(*function))
    throw std::invalid_argument("parse host bridge failed LLVM verification");
  return function;
}
} // namespace kalandra
