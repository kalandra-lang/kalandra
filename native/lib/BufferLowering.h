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
  b.CreateStore(llvm::Constant::getNullValue(packetType), packet);
  if (op.action == BufferAction::NativeCall) {
    if (op.mainThreadOnly) {
      auto check = module.getOrInsertFunction("kalandra_application_is_main_thread_v1", b.getInt32Ty());
      auto *allowed = block("native.thread.valid"), *denied = block("native.thread.invalid");
      b.CreateCondBr(b.CreateIsNotNull(b.CreateCall(check)), allowed, denied);
      b.SetInsertPoint(denied);
      const auto [line, column] = input.source->location(op.source.begin);
      auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                              b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
      b.CreateCall(fault, {ctx, b.getInt64(3), b.getInt64(line), b.getInt64(column)});
      b.CreateBr(abort);
      b.SetInsertPoint(allowed);
    }
    auto *ptr = b.getPtrTy();
    auto *slotType = llvm::StructType::get(context, {ptr, b.getInt64Ty(), b.getInt32Ty()});
    llvm::IRBuilder<> entry(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    auto *slots = entry.CreateAlloca(slotType, b.getInt32(inputs.size()), "native.args");
    auto *adapterType = llvm::FunctionType::get(b.getVoidTy(), {ptr}, false);
    auto *adapter = llvm::Function::Create(adapterType, llvm::GlobalValue::InternalLinkage,
                                           "kalandra.native." + op.nativeSymbol, module);
    llvm::IRBuilder<> call(llvm::BasicBlock::Create(context, "entry", adapter));
    std::vector<llvm::Type *> signature;
    std::vector<llvm::Value *> values;
    for (unsigned i = 0; i < inputs.size(); ++i) {
      auto *slot = b.CreateInBoundsGEP(slotType, slots, b.getInt32(i));
      b.CreateStore(inputs[i], b.CreateStructGEP(slotType, slot, 0));
      auto *value =
          call.CreateLoad(ptr, call.CreateInBoundsGEP(ptr, adapter->getArg(0), call.getInt32(i)));
      llvm::Value *extent = b.getInt64(0);
      if (op.counts[i] >= 0) {
        const auto n = unsigned(op.counts[i]);
        extent = b.CreateZExtOrTrunc(
            b.CreateLoad(types.scalar(input.expressions[op.arguments[n]].type), inputs[n]),
            b.getInt64Ty());
        signature.push_back(ptr);
        values.push_back(value);
      } else {
        auto *scalar = types.scalar(input.expressions[op.arguments[i]].type);
        signature.push_back(scalar);
        values.push_back(call.CreateLoad(scalar, value));
      }
      b.CreateStore(extent, b.CreateStructGEP(slotType, slot, 1));
      b.CreateStore(b.getInt32(op.counts[i] < 0 ? 0
                               : op.writable[i] ? 2
                                                : 1),
                    b.CreateStructGEP(slotType, slot, 2));
    }
    auto callee = module.getOrInsertFunction(
        op.nativeSymbol, llvm::FunctionType::get(b.getVoidTy(), signature, false));
    call.CreateCall(callee, values);
    call.CreateRetVoid();
    auto runtime = module.getOrInsertFunction("kalandra_native_buffers_v1", b.getInt32Ty(), ptr,
                                              ptr, b.getInt32Ty(), ptr);
    auto *kind = b.CreateCall(runtime, {ctx, slots, b.getInt32(inputs.size()), adapter});
    b.CreateStore(kind, b.CreateStructGEP(transport, scratch, 0));
  } else {
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
  }
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
