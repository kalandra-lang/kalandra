#pragma once
#include "RecursiveTypeKey.h"
#include "TaskValueTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace kalandra {
// Emits checked operations on compiler-owned, readable inline frame slots.
// This does not admit recursive source programs or validate arbitrary C pointers.
class TaskRecordOps {
public:
  TaskRecordOps(const TypedModule &input, const TaskValueTypes &types, llvm::Module &module)
      : input_(input), types_(types), module_(module) {}

  llvm::Value *copyManagedField(llvm::IRBuilder<> &builder, llvm::Value *destination,
                                llvm::Value *source, llvm::Value *input, std::size_t id,
                                std::size_t field, llvm::BasicBlock *failed) {
    const auto &type = checkedField(id, field);
    operands(builder, input, destination, failed);
    if (!source || !source->getType()->isPointerTy())
      throw std::invalid_argument("task transfer requires a source invocation pointer");
    if (types_.fieldStorage(id, field) != RecordFieldStorage::ManagedReference || !type.record)
      throw std::invalid_argument("task field copy requires a managed nominal reference");
    guard(builder, input, id, field, destination, failed);
    auto *address = builder.CreateStructGEP(types_.records()[id], input, field);
    auto *node = builder.CreateLoad(builder.getPtrTy(), address);
    const auto [bytes, alignment] = layout(*type.record);
    auto callee =
        module_.getOrInsertFunction("kalandra_recursive_transfer_v1", builder.getPtrTy(),
                                    builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(),
                                    builder.getInt64Ty(), builder.getInt64Ty(), builder.getPtrTy());
    auto *result = builder.CreateCall(callee, {destination, source, node, builder.getInt64(bytes),
                                               builder.getInt64(alignment), key(*type.record)});
    ready(builder, builder.CreateIsNotNull(result), failed, "field.transferred");
    poll(builder, destination, failed);
    return result;
  }

  llvm::Value *project(llvm::IRBuilder<> &builder, llvm::Value *input, std::size_t id,
                       std::size_t field, llvm::Value *context, llvm::BasicBlock *failed) {
    const auto &type = checkedField(id, field);
    operands(builder, input, context, failed);
    guard(builder, input, id, field, context, failed);
    auto *address = builder.CreateStructGEP(types_.records()[id], input, field);
    if (types_.fieldStorage(id, field) == RecordFieldStorage::ManagedReference) {
      auto *node = builder.CreateLoad(builder.getPtrTy(), address);
      const auto [bytes, alignment] = layout(*type.record);
      auto callee = module_.getOrInsertFunction(
          "kalandra_recursive_read_v1", builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy(),
          builder.getInt64Ty(), builder.getInt64Ty(), builder.getPtrTy());
      address = builder.CreateCall(callee, {context, node, builder.getInt64(bytes),
                                            builder.getInt64(alignment), key(*type.record)});
      ready(builder, builder.CreateIsNotNull(address), failed, "field.readable");
      poll(builder, context, failed);
    }
    return builder.CreateLoad(types_.scalar(type), address);
  }

private:
  static void operands(llvm::IRBuilder<> &builder, llvm::Value *input, llvm::Value *context,
                       llvm::BasicBlock *failed) {
    if (!input || !context || !input->getType()->isPointerTy() ||
        !context->getType()->isPointerTy() || !builder.GetInsertBlock() || !failed ||
        builder.GetInsertBlock()->getParent() != failed->getParent())
      throw std::invalid_argument(
          "task field operation requires pointer slots and a local failure exit");
  }
  const ValueType &checkedField(std::size_t id, std::size_t field) const {
    if (id >= input_.records.size() || field >= input_.records[id].fields.size())
      throw std::invalid_argument("task projection requires a checked nominal field");
    return input_.records[id].fields[field].type;
  }
  std::pair<std::uint64_t, std::uint64_t> layout(std::size_t id) const {
    auto *type = types_.records()[id];
    return {module_.getDataLayout().getTypeAllocSize(type).getFixedValue(),
            module_.getDataLayout().getABITypeAlign(type).value()};
  }
  llvm::GlobalVariable *key(std::size_t id) { return recursiveTypeKey(module_, id); }

  static void ready(llvm::IRBuilder<> &builder, llvm::Value *condition, llvm::BasicBlock *failed,
                    const char *name) {
    auto *next =
        llvm::BasicBlock::Create(builder.getContext(), name, builder.GetInsertBlock()->getParent());
    builder.CreateCondBr(condition, next, failed);
    builder.SetInsertPoint(next);
  }
  void poll(llvm::IRBuilder<> &builder, llvm::Value *context, llvm::BasicBlock *failed) {
    auto callee = module_.getOrInsertFunction("kalandra_invocation_ready_v1", builder.getInt1Ty(),
                                              builder.getPtrTy());
    auto *status = builder.CreateCall(callee, {context});
    ready(builder, status, failed, "field.ready");
  }
  void guard(llvm::IRBuilder<> &builder, llvm::Value *input, std::size_t id, std::size_t field,
             llvm::Value *context, llvm::BasicBlock *failed) {
    poll(builder, context, failed);
    const auto &record = input_.records[id];
    if (record.variants.empty())
      return;
    auto *tag = builder.CreateLoad(builder.getInt64Ty(),
                                   builder.CreateStructGEP(types_.records()[id], input, 0));
    llvm::Value *valid =
        builder.CreateAnd(builder.CreateICmpUGE(tag, builder.getInt64(1)),
                          builder.CreateICmpULE(tag, builder.getInt64(record.variants.size())));
    if (const auto tagValue = types_.fieldTag(id, field))
      valid = builder.CreateICmpEQ(tag, builder.getInt64(tagValue));
    auto *invalid = llvm::BasicBlock::Create(builder.getContext(), "field.inactive",
                                             builder.GetInsertBlock()->getParent());
    ready(builder, valid, invalid, "field.active");
    auto *active = builder.GetInsertBlock();
    builder.SetInsertPoint(invalid);
    auto fault = module_.getOrInsertFunction("kalandra_fault_v1", builder.getVoidTy(),
                                             builder.getPtrTy(), builder.getInt64Ty(),
                                             builder.getInt64Ty(), builder.getInt64Ty());
    builder.CreateCall(fault,
                       {context, builder.getInt64(3), builder.getInt64(0), builder.getInt64(0)});
    builder.CreateBr(failed);
    builder.SetInsertPoint(active);
  }
  const TypedModule &input_;
  const TaskValueTypes &types_;
  llvm::Module &module_;
};
} // namespace kalandra
