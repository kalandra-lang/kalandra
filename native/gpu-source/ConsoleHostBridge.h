#pragma once
#include "../lib/ConsoleLowering.h"
#include "llvm/IR/Verifier.h"

namespace kalandra {
// Host supervisor frames keep success, task-failure and declared-error payloads
// in separate typed slots. Construct them with the same physical layouts and
// console packet rules as CPU continuations; no device kernel performs I/O.
inline llvm::Function *emitConsoleHostBridge(const TypedModule &input, llvm::Module &module,
                                             const ConsoleOperation &operation,
                                             const ValueType &result, llvm::StringRef name) {
  if (auto *existing = module.getFunction(name))
    return existing;
  auto made = TaskValueTypes::create(input, module.getContext());
  if (!made)
    throw std::invalid_argument(made.error().message);
  auto &types = *made;
  auto *pointer = llvm::PointerType::getUnqual(module.getContext());
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt1Ty(module.getContext()),
                              {pointer, pointer, pointer, pointer, pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  function->addRetAttr(llvm::Attribute::ZExt);
  // context, operation state, argument, success payload, Failure, IoError.
  auto *context = function->getArg(0);
  auto *state = function->getArg(1);
  auto *argument = function->getArg(2);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", function));
  auto *packetType = types.value(result, "console.host.result");
  auto *packet = b.CreateAlloca(packetType);
  auto *transport = b.CreateAlloca(consoleResultType(types, module.getContext()));
  auto *done = emitConsoleStep(b, module, operation, argument, transport, state, context);
  auto block = [&](const char *label) {
    return llvm::BasicBlock::Create(module.getContext(), label, function);
  };
  auto *finished = block("finished"), *pending = block("pending"), *ready = block("ready");
  auto *complete = block("complete"), *abort = block("faulted");
  b.CreateCondBr(done, finished, pending);
  b.SetInsertPoint(pending);
  b.CreateRet(b.getFalse());
  b.SetInsertPoint(finished);
  auto poll =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  b.CreateCondBr(b.CreateCall(poll, {context}), ready, abort);
  b.SetInsertPoint(ready);
  emitConsolePacket(b, module, input, types, operation, result, packetType, argument, packet,
                    transport, context, complete, abort);
  b.SetInsertPoint(complete);
  for (const auto &[field, destination] :
       {std::pair{1u, 3u}, std::pair{0u, 4u}, std::pair{2u, 5u}}) {
    auto *value = b.CreateLoad(packetType->getElementType(field),
                               b.CreateStructGEP(packetType, packet, field));
    b.CreateStore(value, function->getArg(destination));
  }
  b.CreateRet(b.getTrue());
  b.SetInsertPoint(abort);
  // The caller checks its Invocation/cancellation before publishing any slot.
  b.CreateRet(b.getTrue());
  if (llvm::verifyFunction(*function))
    throw std::invalid_argument("console host bridge failed LLVM verification");
  return function;
}
} // namespace kalandra
