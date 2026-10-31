#pragma once
#include "NativeLayout.h"
#include "RecordLLVMTypes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"

namespace kalandra {
// Checked-IR emitter only. Source/transport admission is enforced separately.
class RecursiveRecordOps {
public:
  RecursiveRecordOps(const TypedModule &input, const RecordStoragePlan &storage,
                     const std::vector<mlir::LLVM::LLVMStructType> &types, mlir::OpBuilder &builder,
                     mlir::ModuleOp module, std::string dataLayout = nativeCpuDataLayout)
      : input_(input), storage_(storage), types_(types), builder_(builder), module_(module),
        variants_(input.records.size()), dataLayout_(std::move(dataLayout)) {
    for (std::size_t id = 0; id < input.records.size(); ++id) {
      variants_[id].resize(input.records[id].fields.size());
      for (std::size_t variant = 0; variant < input.records[id].variants.size(); ++variant)
        for (auto slot : input.records[id].variants[variant].fields)
          variants_[id][slot] = variant + 1;
    }
  }
  std::expected<mlir::Value, std::string> box(mlir::Location loc, mlir::Value value,
                                              std::size_t id) {
    if (id >= types_.size() || value.getType() != types_[id])
      return std::unexpected("recursive box requires its checked nominal physical type");
    struct Pending {
      mlir::Value value, active;
      std::size_t id;
    };
    std::vector<Pending> pending{{value, constant(loc, 1, 1), id}};
    llvm::SmallVector<mlir::Value> dependencies;
    std::size_t work = 0;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      const auto &record = input_.records[current.id];
      if (!record.variants.empty()) {
        auto function =
            declaration("kalandra_recursive_tag_check",
                        {builder_.getI64Type(), builder_.getI64Type(), builder_.getI64Type()});
        auto active =
            mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), current.active);
        mlir::LLVM::CallOp::create(builder_, loc, function,
                                   mlir::ValueRange{extract(loc, current.value, 0),
                                                    constant(loc, record.variants.size()), active});
      }
      for (std::size_t field = 0; field < record.fields.size(); ++field) {
        if (++work > 300000)
          return std::unexpected("recursive box dependency expansion exceeds configured budget");
        const auto &type = record.fields[field].type;
        auto active = current.active;
        if (variants_[current.id][field]) {
          auto tag = extract(loc, current.value, 0);
          auto selected =
              mlir::LLVM::ICmpOp::create(builder_, loc, mlir::LLVM::ICmpPredicate::eq, tag,
                                         constant(loc, variants_[current.id][field]));
          active = mlir::LLVM::AndOp::create(builder_, loc, active, selected);
        }
        auto member = extract(loc, current.value, field);
        if (storage_.fields[current.id][field] == RecordFieldStorage::ManagedReference) {
          const auto [bytes, alignment] = layout(*type.record);
          auto function = declaration("kalandra_recursive_reference_dependency",
                                      {pointer(), builder_.getI64Type(), builder_.getI64Type(),
                                       pointer(), builder_.getI64Type()});
          auto selected = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          dependencies.push_back(
              mlir::LLVM::CallOp::create(builder_, loc, function,
                                         mlir::ValueRange{member, constant(loc, bytes),
                                                          constant(loc, alignment),
                                                          typeKey(loc, *type.record), selected})
                  .getResult());
        } else if (type.record && !type.tensor) {
          pending.push_back({member, active, *type.record});
        } else if (type.builtin == BuiltinValue::String && (!type.tensor || isStringArray(type))) {
          auto descriptor = slot(loc, member.getType(), 1, alignof(void *));
          mlir::LLVM::StoreOp::create(builder_, loc, member, descriptor);
          auto function =
              declaration(isStringArray(type) ? "kalandra_recursive_string_array_dependency"
                                              : "kalandra_recursive_string_dependency",
                          {pointer(), builder_.getI64Type()});
          auto selected = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          dependencies.push_back(mlir::LLVM::CallOp::create(builder_, loc, function,
                                                            mlir::ValueRange{descriptor, selected})
                                     .getResult());
        } else if (type.tensor && (type.builtin == BuiltinValue::Numeric || isBoolArray(type))) {
          auto descriptor = slot(loc, member.getType(), 1, alignof(void *));
          mlir::LLVM::StoreOp::create(builder_, loc, member, descriptor);
          auto selected = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          if (isBoolArray(type)) {
            auto function = declaration("kalandra_recursive_bool_dependency",
                                        {pointer(), builder_.getI64Type()});
            dependencies.push_back(
                mlir::LLVM::CallOp::create(builder_, loc, function,
                                           mlir::ValueRange{descriptor, selected})
                    .getResult());
          } else {
            auto function = declaration(
                "kalandra_recursive_tensor_dependency",
                {pointer(), builder_.getI64Type(), builder_.getI64Type(), builder_.getI64Type()});
            dependencies.push_back(
                mlir::LLVM::CallOp::create(
                    builder_, loc, function,
                    mlir::ValueRange{descriptor, constant(loc, type.shape.size()),
                                     constant(loc, static_cast<unsigned>(type.element)), selected})
                    .getResult());
          }
        } else if (isRecordArray(type)) {
          auto descriptor = slot(loc, member.getType(), 1, alignof(void *));
          mlir::LLVM::StoreOp::create(builder_, loc, member, descriptor);
          const auto [bytes, alignment] = layout(*type.record);
          auto callback = arrayCallback(*type.record);
          auto address = mlir::LLVM::AddressOfOp::create(builder_, loc, callback);
          auto function = declaration("kalandra_recursive_record_array_dependency",
                                      {pointer(), builder_.getI64Type(), builder_.getI64Type(),
                                       pointer(), builder_.getI64Type()});
          auto active64 = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          dependencies.push_back(mlir::LLVM::CallOp::create(
                                     builder_, loc, function,
                                     mlir::ValueRange{descriptor, constant(loc, bytes),
                                                      constant(loc, alignment), address, active64})
                                     .getResult());
        } else if (isFunctionArray(type)) {
          auto descriptor = slot(loc, member.getType(), 1, alignof(void *));
          mlir::LLVM::StoreOp::create(builder_, loc, member, descriptor);
          auto active64 = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          auto function = declaration("kalandra_recursive_function_array_dependency",
                                      {pointer(), builder_.getI64Type()});
          dependencies.push_back(mlir::LLVM::CallOp::create(builder_, loc, function,
                                                            mlir::ValueRange{descriptor, active64})
                                     .getResult());
        } else if (!type.tensor && type.builtin == BuiltinValue::Function) {
          auto function = declaration("kalandra_recursive_function_dependency",
                                      {pointer(), pointer(), builder_.getI64Type()});
          auto active64 = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          dependencies.push_back(
              mlir::LLVM::CallOp::create(
                  builder_, loc, function,
                  mlir::ValueRange{extract(loc, member, 0), extract(loc, member, 1), active64})
                  .getResult());
        } else if (!type.tensor && type.builtin == BuiltinValue::TaskError) {
          auto kind = extract(loc, member, 0);
          auto lower = mlir::LLVM::ICmpOp::create(builder_, loc, mlir::LLVM::ICmpPredicate::uge,
                                                  kind, constant(loc, 1, 32));
          auto upper = mlir::LLVM::ICmpOp::create(builder_, loc, mlir::LLVM::ICmpPredicate::ule,
                                                  kind, constant(loc, 4, 32));
          mlir::Value valid = mlir::LLVM::AndOp::create(builder_, loc, lower, upper);
          const auto variant = variants_[current.id][field];
          std::size_t expected = record.taskErrorFamily ? variant : 0;
          if (variant && !record.errorOrigins.empty()) {
            const auto [family, original] = record.errorOrigins[variant - 1];
            if (input_.records[family].taskErrorFamily)
              expected = original + 1;
          }
          if (expected) {
            auto matches = mlir::LLVM::ICmpOp::create(builder_, loc, mlir::LLVM::ICmpPredicate::eq,
                                                      kind, constant(loc, expected, 32));
            valid = mlir::LLVM::AndOp::create(builder_, loc, valid, matches);
          }
          auto checked = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), valid);
          auto selected = mlir::LLVM::ZExtOp::create(builder_, loc, builder_.getI64Type(), active);
          auto check =
              declaration("kalandra_recursive_tag_check",
                          {builder_.getI64Type(), builder_.getI64Type(), builder_.getI64Type()});
          mlir::LLVM::CallOp::create(builder_, loc, check,
                                     mlir::ValueRange{checked, constant(loc, 1), selected});
        } else if (type.tensor ||
                   (type.builtin != BuiltinValue::Numeric && type.builtin != BuiltinValue::Bool)) {
          return std::unexpected("recursive boxes currently require numeric/Bool/String scalars, "
                                 "numeric collections, Bool/String/record arrays, callables and "
                                 "function arrays, inline "
                                 "supported records and managed recursive references");
        }
      }
    }
    const auto [bytes, alignment] = layout(id);
    auto payload = slot(loc, types_[id], 1, alignment);
    mlir::LLVM::StoreOp::create(builder_, loc, value, payload);
    auto table =
        slot(loc, pointer(), std::max<std::size_t>(1, dependencies.size()), alignof(void *));
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
      auto address = mlir::LLVM::GEPOp::create(builder_, loc, pointer(), pointer(), table,
                                               mlir::ValueRange{constant(loc, index)});
      mlir::LLVM::StoreOp::create(builder_, loc, dependencies[index], address);
    }
    auto key = typeKey(loc, id);
    auto function = declaration("kalandra_recursive_make",
                                {pointer(), builder_.getI64Type(), builder_.getI64Type(), pointer(),
                                 pointer(), builder_.getI64Type()});
    return mlir::LLVM::CallOp::create(builder_, loc, function,
                                      mlir::ValueRange{payload, constant(loc, bytes),
                                                       constant(loc, alignment), key, table,
                                                       constant(loc, dependencies.size())})
        .getResult();
  }
  std::expected<mlir::Value, std::string> read(mlir::Location loc, mlir::Value node,
                                               std::size_t id) {
    if (id >= types_.size() || node.getType() != pointer())
      return std::unexpected("recursive read requires a checked nominal reference");
    const auto [bytes, alignment] = layout(id);
    auto function = declaration("kalandra_recursive_read", {pointer(), builder_.getI64Type(),
                                                            builder_.getI64Type(), pointer()});
    auto data =
        mlir::LLVM::CallOp::create(builder_, loc, function,
                                   mlir::ValueRange{node, constant(loc, bytes),
                                                    constant(loc, alignment), typeKey(loc, id)})
            .getResult();
    return mlir::LLVM::LoadOp::create(builder_, loc, types_[id], data).getResult();
  }

  // Packet/result copy for one boxed nominal root. The checked ABI inserts the
  // receiving invocation and exits on failure/cancellation before publishing output.
  std::expected<mlir::Value, std::string> copyReference(mlir::Location loc, mlir::Value source,
                                                        mlir::Value node, mlir::Value output,
                                                        std::size_t id) {
    if (id >= types_.size() || source.getType() != pointer() || node.getType() != pointer() ||
        output.getType() != pointer())
      return std::unexpected(
          "recursive transfer requires a checked nominal root and pointer slots");
    const auto [bytes, alignment] = layout(id);
    auto function =
        declaration("kalandra_recursive_transfer", {pointer(), pointer(), builder_.getI64Type(),
                                                    builder_.getI64Type(), pointer()});
    auto transferred =
        mlir::LLVM::CallOp::create(builder_, loc, function,
                                   mlir::ValueRange{source, node, constant(loc, bytes),
                                                    constant(loc, alignment), typeKey(loc, id)});
    auto *current = builder_.getInsertionBlock();
    auto *publish = builder_.createBlock(current->getParent());
    auto *done = builder_.createBlock(current->getParent());
    done->addArgument(builder_.getI1Type(), loc);
    builder_.setInsertionPointToEnd(current);
    auto nonnull = mlir::LLVM::ICmpOp::create(builder_, loc, mlir::LLVM::ICmpPredicate::ne,
                                              transferred.getResult(),
                                              mlir::LLVM::ZeroOp::create(builder_, loc, pointer()));
    mlir::LLVM::CondBrOp::create(builder_, loc, nonnull, publish, mlir::ValueRange{}, done,
                                 mlir::ValueRange{constant(loc, 0, 1)});
    builder_.setInsertionPointToStart(publish);
    mlir::LLVM::StoreOp::create(builder_, loc, transferred.getResult(), output);
    mlir::LLVM::BrOp::create(builder_, loc, mlir::ValueRange{constant(loc, 1, 1)}, done);
    builder_.setInsertionPointToStart(done);
    return done->getArgument(0);
  }

  // Declare helpers before generating bodies so array-mediated cycles do not
  // recursively expand the compiler's own call stack.
  std::optional<std::string> finish() {
    for (std::size_t next = 0; next < arrayCallbacks_.size(); ++next) {
      auto [id, function] = arrayCallbacks_[next];
      mlir::OpBuilder::InsertionGuard guard(builder_);
      auto loc = function.getLoc();
      auto *entry = function.addEntryBlock(builder_);
      builder_.setInsertionPointToStart(entry);
      auto value = mlir::LLVM::LoadOp::create(builder_, loc, types_[id], entry->getArgument(0));
      auto boxed = box(loc, value, id);
      if (!boxed)
        return boxed.error();
      mlir::LLVM::ReturnOp::create(builder_, loc, mlir::ValueRange{*boxed});
    }
    return {};
  }

private:
  mlir::LLVM::LLVMFuncOp arrayCallback(std::size_t id) {
    const auto name = "kalandra_recursive_array_cell_" + std::to_string(id);
    if (auto found = module_.lookupSymbol<mlir::LLVM::LLVMFuncOp>(name))
      return found;
    auto function = declaration(name, {pointer()});
    function.setLinkage(mlir::LLVM::Linkage::Internal);
    arrayCallbacks_.push_back({id, function});
    return function;
  }
  mlir::Type pointer() { return mlir::LLVM::LLVMPointerType::get(builder_.getContext()); }
  mlir::Value constant(mlir::Location loc, std::uint64_t value, unsigned bits = 64) {
    auto type = builder_.getIntegerType(bits);
    return mlir::LLVM::ConstantOp::create(builder_, loc, type,
                                          builder_.getIntegerAttr(type, value));
  }
  mlir::Value extract(mlir::Location loc, mlir::Value value, std::size_t field) {
    return mlir::LLVM::ExtractValueOp::create(
        builder_, loc, value, llvm::ArrayRef<std::int64_t>{static_cast<std::int64_t>(field)});
  }
  mlir::Value slot(mlir::Location loc, mlir::Type type, std::size_t count, std::size_t alignment) {
    auto *operation = builder_.getInsertionBlock()->getParentOp();
    while (!mlir::isa<mlir::func::FuncOp, mlir::LLVM::LLVMFuncOp>(operation))
      operation = operation->getParentOp();
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(&operation->getRegion(0).front());
    return mlir::LLVM::AllocaOp::create(builder_, loc, pointer(), type, constant(loc, count),
                                        alignment);
  }
  std::pair<std::uint64_t, std::uint64_t> layout(std::size_t id) {
    llvm::LLVMContext context;
    mlir::LLVM::TypeToLLVMIRTranslator translator(context);
    auto *type = translator.translateType(types_[id]);
    llvm::DataLayout layout(dataLayout_);
    return {layout.getTypeAllocSize(type).getFixedValue(), layout.getABITypeAlign(type).value()};
  }
  mlir::Value typeKey(mlir::Location loc, std::size_t id) {
    const auto name = "kalandra_recursive_type_" + std::to_string(id);
    auto global = module_.lookupSymbol<mlir::LLVM::GlobalOp>(name);
    if (!global) {
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      global = mlir::LLVM::GlobalOp::create(builder_, loc, builder_.getI8Type(), true,
                                            mlir::LLVM::Linkage::Internal, name,
                                            builder_.getI8IntegerAttr(0));
    }
    return mlir::LLVM::AddressOfOp::create(builder_, loc, global);
  }
  mlir::LLVM::LLVMFuncOp declaration(llvm::StringRef name, llvm::ArrayRef<mlir::Type> arguments) {
    if (auto found = module_.lookupSymbol<mlir::LLVM::LLVMFuncOp>(name))
      return found;
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(module_.getBody());
    return mlir::LLVM::LLVMFuncOp::create(builder_, builder_.getUnknownLoc(), name,
                                          mlir::LLVM::LLVMFunctionType::get(pointer(), arguments));
  }
  std::vector<std::pair<std::size_t, mlir::LLVM::LLVMFuncOp>> arrayCallbacks_;
  const TypedModule &input_;
  const RecordStoragePlan &storage_;
  const std::vector<mlir::LLVM::LLVMStructType> &types_;
  mlir::OpBuilder &builder_;
  mlir::ModuleOp module_;
  std::vector<std::vector<std::size_t>> variants_;
  std::string dataLayout_;
};
} // namespace kalandra
