#pragma once
#include "../lib/FileLowering.h"
#include "llvm/IR/Verifier.h"
namespace kalandra {
inline llvm::Function *emitFileHostBridge(const TypedModule &input, llvm::Module &module,
                                          const FileOperation &operation, const ValueType &result,
                                          llvm::StringRef name) {
  auto checked = TaskValueTypes::create(input, module.getContext());
  if (!checked)
    throw std::invalid_argument(checked.error().message);
  auto &types = *checked;
  auto *pointer = llvm::PointerType::getUnqual(module.getContext());
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt1Ty(module.getContext()),
                              {pointer, pointer, pointer, pointer, pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  function->addRetAttr(llvm::Attribute::ZExt);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(module.getContext(), "entry", function));
  auto *context = function->getArg(0), *state = function->getArg(1);
  auto *packetType = types.value(result, "file.host.result");
  auto *packet = b.CreateAlloca(packetType);
  auto *transport = b.CreateAlloca(fileResultType(types, module.getContext()));
  auto *arguments = b.CreateAlloca(fileArgumentsType(types, module.getContext()));
  std::vector<llvm::Value *> inputs;
  for (std::size_t i = 0; i < operation.arguments.size(); ++i)
    inputs.push_back(
        b.CreateLoad(pointer, b.CreateGEP(pointer, function->getArg(2), b.getInt64(i))));
  emitFileArguments(b, types, operation, inputs, arguments);
  auto callee = module.getOrInsertFunction("kalandra_file_step_v1", b.getInt1Ty(), pointer, pointer,
                                           b.getInt32Ty(), pointer, pointer);
  auto *done = b.CreateCall(
      callee, {context, transport, b.getInt32(unsigned(operation.action)), arguments, state});
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, function);
  };
  auto *finished = block("finished"), *pending = block("pending"), *ready = block("ready"),
       *complete = block("complete"), *abort = block("faulted");
  b.CreateCondBr(done, finished, pending);
  b.SetInsertPoint(pending);
  b.CreateRet(b.getFalse());
  b.SetInsertPoint(finished);
  auto poll = module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), pointer);
  b.CreateCondBr(b.CreateCall(poll, {context}), ready, abort);
  b.SetInsertPoint(ready);
  emitFilePacket(b, module, input, types, operation, result, packetType, packet, transport,
                 arguments, context, complete, abort);
  b.SetInsertPoint(complete);
  for (const auto &[field, destination] : {std::pair{1u, 3u}, std::pair{0u, 4u}, std::pair{2u, 5u}})
    b.CreateStore(b.CreateLoad(packetType->getElementType(field),
                               b.CreateStructGEP(packetType, packet, field)),
                  function->getArg(destination));
  b.CreateRet(b.getTrue());
  b.SetInsertPoint(abort);
  b.CreateRet(b.getTrue());
  if (llvm::verifyFunction(*function))
    throw std::invalid_argument("file host bridge failed LLVM verification");
  return function;
}
} // namespace kalandra
