#pragma once
#include "../lib/BufferLowering.h"
#include "llvm/IR/Verifier.h"
namespace kalandra {
inline llvm::Function *emitBufferHostBridge(const TypedModule &input, llvm::Module &module,
                                            const BufferOperation &op, const ValueType &result,
                                            llvm::StringRef name) {
  auto checked = TaskValueTypes::create(input, module.getContext());
  if (!checked)
    throw std::invalid_argument(checked.error().message);
  auto &types = *checked;
  auto &context = module.getContext();
  auto *ptr = llvm::PointerType::getUnqual(context);
  auto *fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {ptr, ptr, ptr, ptr, ptr}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(context, "entry", fn));
  auto *packetType = types.value(result, "buffer.host.result");
  auto *packet = b.CreateAlloca(packetType);
  auto *scratch = b.CreateAlloca(bufferResultType(types, context)),
       *args = b.CreateAlloca(bufferArgumentsType(types, context));
  std::vector<llvm::Value *> inputs;
  for (std::size_t i = 0; i < op.arguments.size(); ++i)
    inputs.push_back(b.CreateLoad(ptr, b.CreateGEP(ptr, fn->getArg(1), b.getInt64(i))));
  auto *done = llvm::BasicBlock::Create(context, "done", fn),
       *abort = llvm::BasicBlock::Create(context, "faulted", fn);
  emitBufferOperation(b, module, input, types, op, result, packetType, inputs, packet, scratch,
                      args, fn->getArg(0), done, abort);
  b.SetInsertPoint(done);
  b.CreateStore(
      b.CreateLoad(packetType->getElementType(1), b.CreateStructGEP(packetType, packet, 1)),
      fn->getArg(2));
  if (result.fallible)
    for (auto [field, dest] : {std::pair{0u, 3u}, std::pair{2u, 4u}})
      b.CreateStore(b.CreateLoad(packetType->getElementType(field),
                                 b.CreateStructGEP(packetType, packet, field)),
                    fn->getArg(dest));
  b.CreateRetVoid();
  b.SetInsertPoint(abort);
  b.CreateRetVoid();
  if (llvm::verifyFunction(*fn))
    throw std::invalid_argument("buffer host bridge failed LLVM verification");
  return fn;
}
} // namespace kalandra
