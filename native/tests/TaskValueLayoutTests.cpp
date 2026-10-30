#include "../lib/NativeLayout.h"
#include "../lib/RecordLLVMTypes.h"
#include "../lib/TaskValueTypes.h"
#include "kalandra/Continuation.h"
#ifndef KALANDRA_TASK_LAYOUT_FRONTEND_ONLY
#include "kalandra/Backend.h"
#endif
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static ValueType scalar(NumericType type = NumericType::Int64) {
  ValueType value;
  value.element = type;
  return value;
}
static ValueType nominal(std::size_t id, bool array = false) {
  ValueType value;
  value.builtin = BuiltinValue::Record;
  value.record = id;
  value.array = value.tensor = array;
  if (array)
    value.shape = {{std::nullopt, 1}};
  return value;
}
static TypedRecord record(std::initializer_list<ValueType> fields) {
  TypedRecord result;
  result.name = "Record";
  for (const auto &field : fields)
    result.fields.push_back({"field", field, {}});
  return result;
}
static CheckResult analyze(std::string source) {
  return check(parse(std::make_shared<SourceFile>("task-layout.k", std::move(source))));
}
int main() {
  try {
    mlir::MLIRContext context;
    context.loadDialect<mlir::LLVM::LLVMDialect>();
    llvm::LLVMContext native;
    llvm::DataLayout layout(nativeCpuDataLayout);
    const auto agree = [&](const TypedModule &input, const SemanticLimits &limits = {}) {
      auto ordinary = lowerRecordTypes(input, context, limits);
      auto task = TaskValueTypes::create(input, native, limits);
      require(ordinary && task, "valid task/ordinary record layout rejected");
      mlir::LLVM::TypeToLLVMIRTranslator translator(native);
      for (std::size_t id = 0; id < input.records.size(); ++id) {
        auto *expected = llvm::cast<llvm::StructType>(translator.translateType((*ordinary)[id]));
        auto *actual = task->records()[id];
        require(actual == expected && actual->isSized(),
                "task and ordinary physical types disagree");
        auto *a = layout.getStructLayout(actual), *b = layout.getStructLayout(expected);
        require(a->getSizeInBytes() == b->getSizeInBytes() &&
                    layout.getABITypeAlign(actual) == layout.getABITypeAlign(expected),
                "task record size/alignment mismatch");
        for (unsigned field = 0; field < actual->getNumElements(); ++field)
          require(a->getElementOffset(field) == b->getElementOffset(field),
                  "task field offset mismatch");
      }
      return std::move(*task);
    };
    TypedModule input;
    input.records = {record({scalar(), scalar(), nominal(0)})};
    input.records[0].variants = {{"Nil", {}, {}}, {"Cons", {}, {1, 2}}};
    auto list = agree(input);
    require(list.records()[0]->getElementType(2)->isPointerTy() &&
                layout.getTypeAllocSize(list.records()[0]) == 24,
            "recursive task list expanded its tail");
    require(list.fieldStorage(0, 2) == RecordFieldStorage::ManagedReference &&
                list.fieldStorage(0, 1) == RecordFieldStorage::Inline && list.fieldTag(0, 0) == 0 &&
                list.fieldTag(0, 1) == 2 && list.fieldTag(0, 2) == 2,
            "task field storage/active-case metadata lost");
    auto *completed = list.value(nominal(0), "completed.list");
    auto pending = nominal(0);
    pending.task = true;
    auto *handle = list.value(pending, "pending.list");
    require(layout.getTypeAllocSize(completed) == 56 && layout.getTypeAllocSize(handle) == 48 &&
                handle->getElementType(1) ==
                    llvm::StructType::get(
                        native, {llvm::Type::getInt32Ty(native), llvm::Type::getInt64Ty(native)}),
            "completed record and task handle wrappers confused");
    input.records = {record({nominal(0, true)})};
    auto tree = agree(input);
    require(layout.getTypeAllocSize(tree.records()[0]) == 40 &&
                tree.records()[0]->getElementType(0)->isStructTy(),
            "recursive tree descriptor changed");
    auto *treeValue = tree.value(nominal(0), "completed.tree");
    auto *packet = llvm::StructType::get(native, {completed, treeValue});
    auto *frame = llvm::StructType::get(
        native, {llvm::Type::getInt32Ty(native), completed, handle->getElementType(1), treeValue});
    auto *frameLayout = layout.getStructLayout(frame);
    require(layout.getTypeAllocSize(packet) == 128 && frameLayout->getSizeInBytes() == 152 &&
                frameLayout->getElementOffset(1) == 8 && frameLayout->getElementOffset(2) == 64 &&
                frameLayout->getElementOffset(3) == 80,
            "recursive packet/frame wrapper padding mismatch");
    llvm::Module module("recursive.frame", native);
    module.setDataLayout(nativeCpuDataLayout);
    llvm::IRBuilder<> builder(native);
    auto *function =
        llvm::Function::Create(llvm::FunctionType::get(builder.getVoidTy(), {}, false),
                               llvm::GlobalValue::InternalLinkage, "frame.initialize", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(native, "entry", function));
    auto *storage = builder.CreateAlloca(frame);
    builder.CreateStore(llvm::Constant::getNullValue(frame), storage);
    builder.CreateLoad(treeValue, builder.CreateStructGEP(frame, storage, 3));
    builder.CreateRetVoid();
    require(!llvm::verifyModule(module, &llvm::errs()), "recursive frame LLVM verification failed");
    input.records = {record({nominal(1)}), record({scalar(), nominal(0)})};
    input.records[1].variants = {{"End", {}, {}}, {"More", {}, {1}}};
    auto mutual = agree(input);
    require(mutual.records()[0]->getElementType(0)->isPointerTy() &&
                mutual.records()[1]->getElementType(1)->isPointerTy(),
            "mutual task reference layout");
    input.records = {record({nominal(1, true)}), record({nominal(0)})};
    auto mixed = agree(input);
    require(mixed.records()[1]->getElementType(0) == mixed.records()[0],
            "array-mediated recursion introduced an extra pointer");
    input.records = {record({nominal(1), nominal(1)}), record({scalar(NumericType::Int8)})};
    agree(input);
    SemanticLimits limited;
    limited.maxValues = 2;
    agree(input, limited);
    limited.maxValues = 1;
    require(!TaskValueTypes::create(input, native, limited), "task expansion budget ignored");
    limited = {};
    limited.maxDepth = 1;
    require(!TaskValueTypes::create(input, native, limited), "task inline depth budget ignored");
    for (const auto &numeric : numericTypes) {
      input.records = {
          record({scalar(NumericType::Int8), scalar(numeric.type), scalar(NumericType::UInt8)})};
      agree(input);
    }
    ValueType text, boolean, callable, failure;
    text.builtin = BuiltinValue::String;
    boolean.builtin = BuiltinValue::Bool;
    callable.builtin = BuiltinValue::Function;
    callable.callable =
        std::make_shared<FunctionSignature>(FunctionSignature{{nominal(0)}, nominal(0)});
    failure.builtin = BuiltinValue::TaskError;
    input.records = {record({text, boolean, callable, failure})};
    agree(input);
    for (auto element : {scalar(), text, boolean, callable, nominal(0)}) {
      element.tensor = element.array = true;
      element.shape = {{std::nullopt, 1}};
      input.records = {record({element})};
      agree(input);
    }
    for (unsigned rank : {0u, 1u, 2u, 128u}) {
      auto tensor = scalar(NumericType::Float64);
      tensor.tensor = true;
      tensor.shape.resize(rank, {2, 0});
      input.records = {record({tensor})};
      agree(input);
    }
    input.records = {record({nominal(1)})};
    require(!TaskValueTypes::create(input, native), "invalid task nominal identity admitted");
    input.records = {record({scalar()})};
    input.records[0].fields[0].type.fallible = true;
    require(!TaskValueTypes::create(input, native), "fallible task record field admitted");
    auto array = nominal(0, true);
    array.shape.push_back({2, 0});
    input.records = {record({array})};
    require(!TaskValueTypes::create(input, native), "invalid task record-array rank admitted");
    input.records = {record({scalar(), scalar()})};
    input.records[0].variants = {{"Bad", {}, {0}}};
    require(!TaskValueTypes::create(input, native), "invalid enum payload partition admitted");
    input.records.clear();
    constexpr std::size_t count = 10000;
    for (std::size_t id = 0; id < count; ++id)
      input.records.push_back(record({nominal((id + 1) % count)}));
    auto deep = agree(input);
    require(deep.records().size() == count &&
                deep.records().back()->getElementType(0)->isPointerTy(),
            "deep task layout expanded nominal recursion");
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
      bool rejected = false;
      try {
        auto type = nominal(invalid == 0 ? count : 0);
        if (invalid == 1)
          type.record.reset();
        if (invalid == 2)
          type.errorRecord = count;
        if (invalid == 2)
          deep.value(type, "invalid.error");
        else
          deep.scalar(type);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      require(rejected, "unavailable task nominal value layout accepted");
    }
    auto source = analyze("enum Chain:\n  End\n  More:\n    next:Chain\nfn main()->int:\n  42\n");
    require(source.ok() && bool(continuation::plan(*source.module)),
            "checked recursive task planning rejected");
    auto taskSource = analyze("type Item:\n  text:String\n  value:int\n"
                              "fn worker(item:Item)->Item:\n  item\nfn main()->int ! TaskError:\n"
                              "  try parallel:\n    value=try await spawn worker(Item "
                              "{text:\"é\",value:42})\n    value.value\n");
    require(taskSource.ok(), "ordinary task source rejected");
    auto taskTypes = agree(*taskSource.module);
    auto fallible = nominal(0);
    fallible.fallible = true;
    fallible.errorRecord = taskSource.module->records.size() - 1;
    require(taskTypes.value(fallible, "fallible.record")->getNumElements() == 3,
            "expected-error record wrapper lost application payload");
#ifndef KALANDRA_TASK_LAYOUT_FRONTEND_ONLY
    require(bool(lowerNative(*taskSource.module)), "ordinary worker lowering regressed");
#endif
    std::cout << "task value layouts passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
