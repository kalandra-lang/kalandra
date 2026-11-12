#pragma once
#include "FileLowering.h"
namespace kalandra {
inline llvm::StructType *bufferArgumentsType(const TaskValueTypes &types, llvm::LLVMContext &ctx) {
  return llvm::StructType::get(ctx, {fileHandleType(types, ctx), fileBytesType(types),
                                     llvm::Type::getInt64Ty(ctx), llvm::Type::getInt8Ty(ctx),
                                     llvm::Type::getInt64Ty(ctx), llvm::Type::getInt64Ty(ctx)});
}
inline llvm::StructType *bufferResultType(const TaskValueTypes &types, llvm::LLVMContext &ctx) {
  return llvm::StructType::get(ctx, {llvm::Type::getInt32Ty(ctx), llvm::Type::getInt64Ty(ctx),
                                     fileHandleType(types, ctx), fileBytesType(types)});
}
inline void emitBufferOperation(llvm::IRBuilder<> &b, llvm::Module &module,
                                const TypedModule &input, const TaskValueTypes &types,
                                const BufferOperation &op, const ValueType &result,
                                llvm::StructType *packetType,
                                const std::vector<llvm::Value *> &inputs, llvm::Value *packet,
                                llvm::Value *scratch, llvm::Value *arguments, llvm::Value *ctx,
                                llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  auto &context = b.getContext();
  auto *fn = b.GetInsertBlock()->getParent();
  auto block = [&](const char *name) { return llvm::BasicBlock::Create(context, name, fn); };
  auto *args = bufferArgumentsType(types, context), *transport = bufferResultType(types, context);
  b.CreateStore(llvm::Constant::getNullValue(args), arguments);
  auto put = [&](unsigned field, llvm::Value *input) {
    b.CreateStore(b.CreateLoad(args->getElementType(field), input),
                  b.CreateStructGEP(args, arguments, field));
  };
  if (op.action == BufferAction::Create)
    put(2, inputs.at(0));
  else {
    put(0, inputs.at(0));
    if (inputs.size() > 1) {
      if (op.action == BufferAction::CheckIndex || op.action == BufferAction::ReadByte ||
          op.action == BufferAction::WriteByte) {
        const auto &index = input.expressions.at(op.arguments.at(1)).type;
        llvm::Value *word = b.CreateLoad(types.scalar(index), inputs.at(1));
        word = b.CreateIntCast(word, b.getInt64Ty(), !isUnsigned(index.element));
        b.CreateStore(word, b.CreateStructGEP(args, arguments, 2));
      } else
        put(op.action == BufferAction::Append ? 1 : 2, inputs.at(1));
    }
    if (op.action == BufferAction::WriteByte)
      put(3, inputs.at(2));
  }
  const auto [line, column] = input.source->location(op.source.begin);
  b.CreateStore(b.getInt64(line), b.CreateStructGEP(args, arguments, 4));
  b.CreateStore(b.getInt64(column), b.CreateStructGEP(args, arguments, 5));
  b.CreateStore(llvm::Constant::getNullValue(packetType), packet);
  auto runtime = module.getOrInsertFunction("kalandra_buffer_v1", b.getVoidTy(), b.getPtrTy(),
                                            b.getPtrTy(), b.getInt32Ty(), b.getPtrTy());
  b.CreateCall(runtime, {ctx, scratch, b.getInt32(unsigned(op.action)), arguments});
  auto ready =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  auto *dispatch = block("buffer.ready"), *ok = block("buffer.success"),
       *bad = block("buffer.invalid");
  b.CreateCondBr(b.CreateCall(ready, {ctx}), dispatch, abort);
  b.SetInsertPoint(dispatch);
  auto *kind = b.CreateLoad(b.getInt32Ty(), b.CreateStructGEP(transport, scratch, 0));
  auto *select = b.CreateSwitch(kind, bad, result.fallible ? 3 : 1);
  select->addCase(b.getInt32(0), ok);
  if (result.fallible)
    for (unsigned tag : {1u, 2u}) {
      auto *error = block("buffer.error");
      select->addCase(b.getInt32(tag), error);
      b.SetInsertPoint(error);
      b.CreateStore(b.getInt64(tag),
                    b.CreateStructGEP(types.records()[*result.errorRecord],
                                      b.CreateStructGEP(packetType, packet, 2), 0));
      b.CreateBr(next);
    }
  b.SetInsertPoint(bad);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
  b.CreateCall(fault, {ctx, b.getInt64(3), b.getInt64(0), b.getInt64(0)});
  b.CreateBr(abort);
  b.SetInsertPoint(ok);
  auto *out = b.CreateStructGEP(packetType, packet, 1);
  unsigned field = 0;
  if (op.action == BufferAction::Create)
    field = 2;
  else if (op.action == BufferAction::Length || op.action == BufferAction::Capacity ||
           op.action == BufferAction::ReadByte || op.action == BufferAction::CheckIndex)
    field = 1;
  else if (op.action == BufferAction::ToArray || op.action == BufferAction::IntoArray)
    field = 3;
  if (field) {
    llvm::Value *value = b.CreateLoad(transport->getElementType(field),
                                      b.CreateStructGEP(transport, scratch, field));
    if (op.action == BufferAction::ReadByte)
      value = b.CreateTrunc(value, b.getInt8Ty());
    b.CreateStore(value, out);
  }
  b.CreateBr(next);
}
} // namespace kalandra
