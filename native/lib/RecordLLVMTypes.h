#pragma once
#include "RecordPhysicalLayout.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"

namespace kalandra {
// Physical type construction only. Callers must separately enforce execution
// admission: managed-reference constructors and ownership are not implemented.
inline std::expected<std::vector<mlir::LLVM::LLVMStructType>, Diagnostic>
lowerRecordTypes(const TypedModule &module, mlir::MLIRContext &context,
                 const SemanticLimits &limits = {}) {
  auto layout = planRecordPhysicalLayout(module, limits);
  if (!layout)
    return std::unexpected(layout.error());
  mlir::Builder builder(&context);
  const auto ptr = mlir::LLVM::LLVMPointerType::get(&context);
  std::vector<mlir::LLVM::LLVMStructType> types(module.records.size());
  for (auto id : layout->order) {
    llvm::SmallVector<mlir::Type> fields;
    for (std::size_t field = 0; field < module.records[id].fields.size(); ++field) {
      const auto &type = module.records[id].fields[field].type;
      if (layout->storage.fields[id][field] == RecordFieldStorage::ManagedReference) {
        fields.push_back(ptr);
      } else if (type.tensor) {
        const auto dims = mlir::LLVM::LLVMArrayType::get(builder.getI64Type(), type.shape.size());
        fields.push_back(mlir::LLVM::LLVMStructType::getLiteral(
            &context, {ptr, ptr, builder.getI64Type(), dims, dims}));
      } else if (type.record) {
        fields.push_back(types[*type.record]);
      } else if (type.builtin == BuiltinValue::TaskError) {
        fields.push_back(mlir::LLVM::LLVMStructType::getLiteral(
            &context, {builder.getI32Type(), builder.getI64Type(), builder.getI64Type(),
                       builder.getI64Type()}));
      } else if (type.builtin == BuiltinValue::Function) {
        fields.push_back(mlir::LLVM::LLVMStructType::getLiteral(&context, {ptr, ptr}));
      } else if (type.builtin == BuiltinValue::String) {
        fields.push_back(
            mlir::LLVM::LLVMStructType::getLiteral(&context, {ptr, ptr, builder.getI64Type()}));
      } else if (type.builtin == BuiltinValue::Bool) {
        fields.push_back(builder.getI1Type());
      } else if (isInteger(type.element)) {
        fields.push_back(builder.getIntegerType(describeNumeric(type.element)->bits));
      } else {
        fields.push_back(type.element == NumericType::Float64 ? mlir::Type(builder.getF64Type())
                                                              : mlir::Type(builder.getF32Type()));
      }
    }
    types[id] = mlir::LLVM::LLVMStructType::getLiteral(&context, fields);
  }
  return types;
}
} // namespace kalandra
