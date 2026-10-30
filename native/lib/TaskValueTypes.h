#pragma once
#include "RecordPhysicalLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include <span>
#include <stdexcept>

namespace kalandra {
// Physical layouts only. Task admission, lifetime/copy callbacks and continuation
// instruction lowering remain separate checks; a finite type is not executable.
class TaskValueTypes {
public:
  static std::expected<TaskValueTypes, Diagnostic>
  create(const TypedModule &module, llvm::LLVMContext &context, const SemanticLimits &limits = {}) {
    auto plan = planRecordPhysicalLayout(module, limits);
    if (!plan)
      return std::unexpected(plan.error());
    TaskValueTypes result(context, module.records.size());
    for (auto id : plan->order) {
      std::vector<llvm::Type *> fields;
      for (std::size_t field = 0; field < module.records[id].fields.size(); ++field)
        fields.push_back(plan->storage.fields[id][field] == RecordFieldStorage::ManagedReference
                             ? result.pointer()
                             : result.scalar(module.records[id].fields[field].type));
      result.records_[id] = llvm::StructType::get(context, fields);
    }
    result.storage_ = std::move(plan->storage);
    result.fieldTags_.resize(module.records.size());
    for (std::size_t id = 0; id < module.records.size(); ++id) {
      result.fieldTags_[id].resize(module.records[id].fields.size());
      for (std::size_t variant = 0; variant < module.records[id].variants.size(); ++variant)
        for (auto field : module.records[id].variants[variant].fields)
          result.fieldTags_[id][field] = variant + 1;
    }
    return result;
  }
  llvm::Type *scalar(const ValueType &type) const {
    if (type.builtin == BuiltinValue::Function && !type.tensor)
      return llvm::StructType::get(*context_, {pointer(), pointer()});
    if (type.tensor) {
      auto *dimensions = llvm::ArrayType::get(integer(64), type.shape.size());
      return llvm::StructType::get(*context_,
                                   {pointer(), pointer(), integer(64), dimensions, dimensions});
    }
    if (type.builtin == BuiltinValue::Record) {
      if (!type.record)
        throw std::invalid_argument("task value requires a checked nominal identity");
      return record(*type.record);
    }
    if (type.builtin == BuiltinValue::TaskError)
      return failure();
    if (type.builtin == BuiltinValue::FaultKind)
      return integer(32);
    if (type.builtin == BuiltinValue::Bool)
      return integer(1);
    if (type.builtin == BuiltinValue::String)
      return llvm::StructType::get(*context_, {pointer(), pointer(), integer(64)});
    return isInteger(type.element)
               ? static_cast<llvm::Type *>(integer(describeNumeric(type.element)->bits))
           : type.element == NumericType::Float64 ? llvm::Type::getDoubleTy(*context_)
                                                  : llvm::Type::getFloatTy(*context_);
  }
  llvm::StructType *value(const ValueType &type, llvm::StringRef name) const {
    llvm::Type *payload =
        type.task ? llvm::StructType::get(*context_, {integer(32), integer(64)}) : scalar(type);
    std::vector<llvm::Type *> fields{failure(), payload};
    if (type.errorRecord)
      fields.push_back(record(*type.errorRecord));
    return llvm::StructType::create(*context_, fields, name);
  }
  std::span<llvm::StructType *const> records() const { return records_; }
  std::size_t fieldTag(std::size_t id, std::size_t field) const {
    return fieldTags_.at(id).at(field);
  }
  RecordFieldStorage fieldStorage(std::size_t id, std::size_t field) const {
    return storage_.fields.at(id).at(field);
  }

private:
  TaskValueTypes(llvm::LLVMContext &context, std::size_t count)
      : context_(&context), records_(count) {}
  llvm::StructType *record(std::size_t id) const {
    if (id >= records_.size() || !records_[id])
      throw std::invalid_argument("task nominal layout is unavailable");
    return records_[id];
  }
  llvm::PointerType *pointer() const { return llvm::PointerType::getUnqual(*context_); }
  llvm::IntegerType *integer(unsigned bits) const {
    return llvm::IntegerType::get(*context_, bits);
  }
  llvm::StructType *failure() const {
    return llvm::StructType::get(*context_, {integer(32), integer(64), integer(64), integer(64)});
  }
  llvm::LLVMContext *context_;
  std::vector<llvm::StructType *> records_;
  RecordStoragePlan storage_;
  std::vector<std::vector<std::size_t>> fieldTags_;
};
} // namespace kalandra
