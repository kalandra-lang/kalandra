#pragma once
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include <algorithm>
#include <stdexcept>
namespace kalandra {
inline llvm::StructType *fileBytesType(const TaskValueTypes &types) {
  ValueType bytes;
  bytes.tensor = bytes.array = true;
  bytes.shape.resize(1);
  bytes.element = NumericType::UInt8;
  return llvm::cast<llvm::StructType>(types.scalar(bytes));
}
inline llvm::StructType *fileHandleType(const TaskValueTypes &types, llvm::LLVMContext &ctx) {
  return llvm::StructType::get(ctx, std::vector<llvm::Type *>{fileBytesType(types)});
}
inline llvm::StructType *fileArgumentsType(const TaskValueTypes &types, llvm::LLVMContext &ctx) {
  ValueType text;
  text.builtin = BuiltinValue::String;
  return llvm::StructType::get(ctx, {fileHandleType(types, ctx), types.scalar(text),
                                     fileBytesType(types), llvm::Type::getInt64Ty(ctx),
                                     llvm::Type::getInt64Ty(ctx)});
}
inline llvm::StructType *fileResultType(const TaskValueTypes &types, llvm::LLVMContext &ctx) {
  ValueType text;
  text.builtin = BuiltinValue::String;
  return llvm::StructType::get(ctx, {llvm::Type::getInt32Ty(ctx), llvm::Type::getInt32Ty(ctx),
                                     types.scalar(text), fileHandleType(types, ctx),
                                     fileBytesType(types), llvm::Type::getInt64Ty(ctx)});
}
inline void emitFileArguments(llvm::IRBuilder<> &b, const TaskValueTypes &types,
                              const FileOperation &op, const std::vector<llvm::Value *> &inputs,
                              llvm::Value *arguments) {
  auto *layout = fileArgumentsType(types, b.getContext());
  b.CreateStore(llvm::Constant::getNullValue(layout), arguments);
  const auto copy = [&](unsigned field, llvm::Value *input) {
    auto *type = layout->getElementType(field);
    b.CreateStore(b.CreateLoad(type, input), b.CreateStructGEP(layout, arguments, field));
  };
  if (op.action == FileAction::Open) {
    copy(1, inputs.at(0));
    auto *mode = b.CreateSub(b.CreateLoad(b.getInt64Ty(), inputs.at(1)), b.getInt64(1));
    b.CreateStore(mode, b.CreateStructGEP(layout, arguments, 4));
    return;
  }
  copy(0, inputs.at(0));
  if (inputs.size() > 1) {
    const bool bytes = op.action == FileAction::Write || op.action == FileAction::WriteAll ||
                       op.action == FileAction::ReadInto;
    const bool text = op.action == FileAction::WriteText || op.action == FileAction::WriteLine;
    copy(text ? 1 : bytes ? 2 : 3, inputs.at(1));
  }
}
inline void emitFilePacket(llvm::IRBuilder<> &b, llvm::Module &module, const TypedModule &input,
                           const TaskValueTypes &types, const FileOperation &op,
                           const ValueType &result, llvm::StructType *resultType,
                           llvm::Value *output, llvm::Value *scratch, llvm::Value *arguments,
                           llvm::Value *context, llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  if (!result.errorRecord ||
      input.records.at(*result.errorRecord).standard != StandardRecord::FsError)
    throw std::invalid_argument("filesystem operation lost its nominal error identity");
  auto *transport = fileResultType(types, module.getContext());
  auto *args = fileArgumentsType(types, module.getContext());
  auto *parent = b.GetInsertBlock()->getParent();
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, parent);
  };
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto *invalid = block("file.invalid");
  auto *success = block("file.success");
  auto *end = block("file.end");
  auto *kind = b.CreateLoad(b.getInt32Ty(), b.CreateStructGEP(transport, scratch, 0));
  auto *dispatch = b.CreateSwitch(kind, invalid, 7);
  dispatch->addCase(b.getInt32(0), success);
  dispatch->addCase(b.getInt32(1), end);
  auto *errorType = types.records()[*result.errorRecord];
  const auto storeField = [&](std::size_t recordId, llvm::Value *record, const char *name,
                              llvm::Value *value) {
    const auto &fields = input.records.at(recordId).fields;
    const auto found =
        std::ranges::find_if(fields, [&](const auto &field) { return field.name == name; });
    if (found == fields.end())
      throw std::invalid_argument("filesystem standard field missing");
    b.CreateStore(value,
                  b.CreateStructGEP(types.records()[recordId], record, found - fields.begin()));
  };
  const auto loadTransport = [&](unsigned field) {
    return b.CreateLoad(transport->getElementType(field),
                        b.CreateStructGEP(transport, scratch, field));
  };
  for (unsigned kind = 2; kind <= 6; ++kind) {
    auto *error = block("file.error");
    dispatch->addCase(b.getInt32(kind), error);
    b.SetInsertPoint(error);
    auto *packet = b.CreateStructGEP(resultType, output, 2);
    b.CreateStore(b.getInt64(kind - 1), b.CreateStructGEP(errorType, packet, 0));
    if (kind == 2) {
      storeField(*result.errorRecord, packet, "code", loadTransport(1));
      storeField(*result.errorRecord, packet, "message", loadTransport(2));
    } else if (kind == 4) {
      storeField(*result.errorRecord, packet, "limit",
                 b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(args, arguments, 3)));
    } else if (kind == 5) {
      storeField(*result.errorRecord, packet, "expected",
                 b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(args, arguments, 3)));
      storeField(*result.errorRecord, packet, "actual", loadTransport(5));
    }
    b.CreateBr(next);
  }
  b.SetInsertPoint(success);
  auto *value = b.CreateStructGEP(resultType, output, 1);
  switch (op.action) {
  case FileAction::Open:
  case FileAction::BufferedReader:
  case FileAction::BufferedWriter:
    b.CreateStore(loadTransport(3), value);
    break;
  case FileAction::Read:
  case FileAction::ReadExact:
    b.CreateStore(loadTransport(4), value);
    break;
  case FileAction::ReadInto:
  case FileAction::Write:
    b.CreateStore(loadTransport(5), value);
    break;
  case FileAction::ReadLine:
    if (!result.record || input.records.at(*result.record).standard != StandardRecord::Line)
      throw std::invalid_argument("filesystem line operation lost its nominal Line identity");
    b.CreateStore(b.getInt64(2), b.CreateStructGEP(types.records()[*result.record], value, 0));
    storeField(*result.record, value, "text", loadTransport(2));
    break;
  default:
    break;
  }
  b.CreateBr(next);
  b.SetInsertPoint(end);
  if (op.action == FileAction::ReadLine) {
    b.CreateStore(b.getInt64(1), b.CreateStructGEP(types.records()[*result.record],
                                                   b.CreateStructGEP(resultType, output, 1), 0));
    b.CreateBr(next);
  } else
    b.CreateBr(invalid);
  b.SetInsertPoint(invalid);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
  b.CreateCall(fault, {context, b.getInt64(3), b.getInt64(0), b.getInt64(0)});
  b.CreateBr(abort);
}
inline void emitFileOperation(llvm::IRBuilder<> &b, llvm::Module &module, const TypedModule &input,
                              const TaskValueTypes &types, const FileOperation &op,
                              const ValueType &result, llvm::StructType *resultType,
                              const std::vector<llvm::Value *> &inputs, llvm::Value *output,
                              llvm::Value *scratch, llvm::Value *arguments, llvm::Value *state,
                              llvm::Value *context, llvm::Value *task, llvm::Value *pc,
                              std::size_t instruction, llvm::BasicBlock *next,
                              llvm::BasicBlock *leave, llvm::BasicBlock *abort, bool closed) {
  emitFileArguments(b, types, op, inputs, arguments);
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto *parent = b.GetInsertBlock()->getParent();
  if (closed) {
    auto callee = module.getOrInsertFunction("kalandra_file_v1", b.getVoidTy(), b.getPtrTy(),
                                             b.getPtrTy(), b.getInt32Ty(), b.getPtrTy());
    b.CreateCall(callee, {context, scratch, b.getInt32(unsigned(op.action)), arguments});
  } else {
    auto callee =
        module.getOrInsertFunction("kalandra_file_step_v1", b.getInt1Ty(), b.getPtrTy(),
                                   b.getPtrTy(), b.getInt32Ty(), b.getPtrTy(), b.getPtrTy());
    auto *done =
        b.CreateCall(callee, {context, scratch, b.getInt32(unsigned(op.action)), arguments, state});
    auto *finished = llvm::BasicBlock::Create(module.getContext(), "file.finished", parent);
    auto *pending = llvm::BasicBlock::Create(module.getContext(), "file.pending", parent);
    b.CreateCondBr(done, finished, pending);
    b.SetInsertPoint(pending);
    b.CreateStore(b.getInt32(instruction), pc);
    auto pause = module.getOrInsertFunction("kalandra_file_pause_v1", b.getVoidTy(), b.getPtrTy(),
                                            b.getPtrTy());
    b.CreateCall(pause, {task, b.CreateLoad(b.getPtrTy(), state)});
    b.CreateBr(leave);
    b.SetInsertPoint(finished);
  }
  auto *ready = llvm::BasicBlock::Create(module.getContext(), "file.ready", parent);
  auto poll =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  b.CreateCondBr(b.CreateCall(poll, {context}), ready, abort);
  b.SetInsertPoint(ready);
  emitFilePacket(b, module, input, types, op, result, resultType, output, scratch, arguments,
                 context, next, abort);
}
} // namespace kalandra
