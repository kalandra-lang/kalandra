#pragma once
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
namespace kalandra {
inline llvm::StructType *parseResultType(llvm::LLVMContext &context) {
  return llvm::StructType::get(context,
                               {llvm::Type::getInt64Ty(context), llvm::Type::getInt64Ty(context),
                                llvm::Type::getInt32Ty(context)});
}
inline void emitParseOperation(llvm::IRBuilder<> &b, llvm::Module &module, const TypedModule &input,
                               const TaskValueTypes &types, const ParseOperation &op,
                               const ValueType &result, llvm::StructType *resultType,
                               llvm::Value *argument, llvm::Value *output, llvm::Value *scratch,
                               llvm::Value *ctx, llvm::BasicBlock *next, llvm::BasicBlock *abort) {
  auto *transport = parseResultType(module.getContext());
  auto *parent = b.GetInsertBlock()->getParent();
  const auto block = [&](const char *name) {
    return llvm::BasicBlock::Create(module.getContext(), name, parent);
  };
  b.CreateStore(llvm::Constant::getNullValue(resultType), output);
  auto callee = module.getOrInsertFunction("kalandra_parse_scalar_v1", b.getVoidTy(), b.getPtrTy(),
                                           b.getPtrTy(), b.getInt32Ty(), b.getPtrTy());
  b.CreateCall(callee, {ctx, scratch, b.getInt32(unsigned(op.kind)), argument});
  auto ready =
      module.getOrInsertFunction("kalandra_invocation_ready_v1", b.getInt1Ty(), b.getPtrTy());
  auto *dispatch = block("parse.ready"), *success = block("parse.success"),
       *invalid = block("parse.invalid"), *range = block("parse.range"),
       *corrupt = block("parse.corrupt");
  b.CreateCondBr(b.CreateCall(ready, {ctx}), dispatch, abort);
  b.SetInsertPoint(dispatch);
  auto *kind = b.CreateLoad(b.getInt32Ty(), b.CreateStructGEP(transport, scratch, 2));
  auto *select = b.CreateSwitch(kind, corrupt, 3);
  select->addCase(b.getInt32(0), success);
  select->addCase(b.getInt32(1), invalid);
  select->addCase(b.getInt32(2), range);
  b.SetInsertPoint(success);
  llvm::Value *bits = b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(transport, scratch, 0));
  const auto scalar = types.scalar(result);
  if (scalar->isFloatingPointTy()) {
    if (scalar->isFloatTy())
      bits = b.CreateTrunc(bits, b.getInt32Ty());
    bits = b.CreateBitCast(bits, scalar);
  } else if (scalar->getIntegerBitWidth() < 64)
    bits = b.CreateTrunc(bits, scalar);
  b.CreateStore(bits, b.CreateStructGEP(resultType, output, 1));
  b.CreateBr(next);
  auto *errorType = types.records()[*result.errorRecord];
  const auto packet = [&](unsigned tag) {
    auto *error = b.CreateStructGEP(resultType, output, 2);
    b.CreateStore(b.getInt64(tag), b.CreateStructGEP(errorType, error, 0));
    return error;
  };
  b.SetInsertPoint(invalid);
  const auto field = input.records[*result.errorRecord].variants[0].fields[0];
  b.CreateStore(b.CreateLoad(b.getInt64Ty(), b.CreateStructGEP(transport, scratch, 1)),
                b.CreateStructGEP(errorType, packet(1), field));
  b.CreateBr(next);
  b.SetInsertPoint(range);
  packet(2);
  b.CreateBr(next);
  b.SetInsertPoint(corrupt);
  auto fault = module.getOrInsertFunction("kalandra_fault_v1", b.getVoidTy(), b.getPtrTy(),
                                          b.getInt64Ty(), b.getInt64Ty(), b.getInt64Ty());
  b.CreateCall(fault, {ctx, b.getInt64(3), b.getInt64(0), b.getInt64(0)});
  b.CreateBr(abort);
}
} // namespace kalandra
