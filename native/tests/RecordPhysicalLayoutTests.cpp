#include "../lib/RecordLLVMTypes.h"
#ifndef KALANDRA_PHYSICAL_LAYOUT_FRONTEND_ONLY
#include "kalandra/Backend.h"
#endif
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static ValueType scalar(NumericType element = NumericType::Int64) {
  ValueType value;
  value.element = element;
  return value;
}
static ValueType nominal(std::size_t id, bool array = false) {
  ValueType value;
  value.builtin = BuiltinValue::Record;
  value.record = id;
  value.array = value.tensor = array;
  if (array)
    value.shape.push_back({std::nullopt, 1});
  return value;
}
static TypedRecord record(std::initializer_list<ValueType> fields) {
  TypedRecord value;
  value.name = "Test";
  for (const auto &type : fields)
    value.fields.push_back({"field", type, {}});
  return value;
}
static CheckResult analyze(std::string source, SemanticLimits limits = {}) {
  return check(parse(std::make_shared<SourceFile>("layout.k", std::move(source))), limits);
}
int main() {
  try {
    mlir::MLIRContext context;
    context.loadDialect<mlir::LLVM::LLVMDialect>();
    mlir::Builder builder(&context);
    auto ptr = mlir::LLVM::LLVMPointerType::get(&context);
    TypedModule module;
    module.records = {record({scalar(), scalar(), nominal(0)})};
    module.records[0].variants = {{"Nil", {}, {}}, {"Cons", {}, {1, 2}}};
    auto list = lowerRecordTypes(module, context);
    require(bool(list), "recursive list physical type rejected");
    require(recordInlineUnits(module, 0) == 3, "recursive literal storage units expanded a cycle");
    auto body = (*list)[0].getBody();
    require(body.size() == 3 && body[0].isInteger(64) && body[1].isInteger(64) && body[2] == ptr,
            "recursive list does not use finite pointer field");
    llvm::LLVMContext llvmContext;
    mlir::LLVM::TypeToLLVMIRTranslator translator(llvmContext);
    auto *llvmList = translator.translateType((*list)[0]);
    require(llvmList && llvmList->isSized() &&
                llvm::DataLayout("e-p:64:64-i64:64").getTypeAllocSize(llvmList) == 24,
            "recursive physical type is not a sized LLVM aggregate");
    require(bool(recordLayoutError(module)) && !numericCapabilityError(module, NumericTarget::Cpu),
            "qualified recursive physical layout rejected");
#ifndef KALANDRA_PHYSICAL_LAYOUT_FRONTEND_ONLY
    require(!lowerNative(module), "declaration-only module admitted without an entry");
#endif
    module.records = {record({nominal(1)}), record({scalar(), nominal(0)})};
    module.records[1].variants = {{"End", {}, {}}, {"More", {}, {1}}};
    auto mutual = lowerRecordTypes(module, context);
    require(mutual && (*mutual)[0].getBody()[0] == ptr && (*mutual)[1].getBody()[1] == ptr,
            "mutual recursive physical references");
    module.records = {record({nominal(1, true)}), record({nominal(0)})};
    auto mixed = lowerRecordTypes(module, context);
    require(recordInlineUnits(module, 0) == 5 && recordInlineUnits(module, 1) == 5,
            "array-mediated literal units expanded elements");
    require(mixed && (*mixed)[1].getBody()[0] == (*mixed)[0] &&
                mlir::isa<mlir::LLVM::LLVMStructType>((*mixed)[0].getBody()[0]),
            "descriptor cycle introduced redundant reference");
    module.records = {record({nominal(1), nominal(1)}), record({scalar()})};
    auto physical = planRecordPhysicalLayout(module);
    auto acyclic = lowerRecordTypes(module, context);
    require(physical && physical->units == std::vector<std::size_t>({2, 1}) &&
                physical->order == std::vector<std::size_t>({1, 0}) && acyclic &&
                (*acyclic)[0].getBody()[0] == (*acyclic)[1],
            "inline sharing/order changed");
    SemanticLimits small;
    small.maxValues = 2;
    require(bool(planRecordPhysicalLayout(module, small)), "exact expansion budget rejected");
    small.maxValues = 1;
    require(!planRecordPhysicalLayout(module, small), "expanded duplicate child budget ignored");
    small = {};
    small.maxDepth = 1;
    require(!planRecordPhysicalLayout(module, small), "inline depth budget ignored");
    ValueType task, function, string, boolean;
    task.builtin = BuiltinValue::TaskError;
    function.builtin = BuiltinValue::Function;
    function.callable = std::make_shared<FunctionSignature>(FunctionSignature{{}, scalar()});
    string.builtin = BuiltinValue::String;
    boolean.builtin = BuiltinValue::Bool;
    module.records = {record({task, function, string, boolean, scalar(NumericType::Float64)})};
    auto leaves = lowerRecordTypes(module, context);
    auto leafPlan = planRecordPhysicalLayout(module);
    require(leaves && leafPlan && leafPlan->units[0] == 11 &&
                (*leaves)[0].getBody()[0] ==
                    mlir::LLVM::LLVMStructType::getLiteral(
                        &context, {builder.getI32Type(), builder.getI64Type(), builder.getI64Type(),
                                   builder.getI64Type()}) &&
                (*leaves)[0].getBody()[1] ==
                    mlir::LLVM::LLVMStructType::getLiteral(&context, {ptr, ptr}) &&
                (*leaves)[0].getBody()[2] == mlir::LLVM::LLVMStructType::getLiteral(
                                                 &context, {ptr, ptr, builder.getI64Type()}) &&
                (*leaves)[0].getBody()[3].isInteger(1) && (*leaves)[0].getBody()[4].isF64(),
            "existing leaf representations changed");
    for (const auto &description : numericTypes) {
      module.records = {record({scalar(description.type)})};
      auto lowered = lowerRecordTypes(module, context);
      require(bool(lowered), "numeric field rejected");
      auto type = (*lowered)[0].getBody()[0];
      require(isInteger(description.type) ? type.isInteger(description.bits)
              : description.bits == 32    ? type.isF32()
                                          : type.isF64(),
              "numeric width lost");
    }
    module.records[0].fields[0].type.device = 0;
    require(!lowerRecordTypes(module, context), "invalid placed field admitted");
    module.records = {record({nominal(1)})};
    require(!lowerRecordTypes(module, context), "invalid nominal field admitted");
    // Physical expansion is iterative; only actual inline depth is limited.
    module.records.clear();
    constexpr std::size_t count = 2000;
    for (std::size_t id = 0; id < count; ++id)
      module.records.push_back(record({nominal((id + 1) % count)}));
    auto deep = lowerRecordTypes(module, context);
    require(deep && deep->size() == count && deep->back().getBody()[0] == ptr,
            "deep recursive layout used inline recursion");
    // Resolution no longer mistakes an opaque callback contract for a storage cycle.
    auto callable = analyze("type Node:\n  visit:fn(Node)->int\nfn visit(node:Node)->int:\n  42\n"
                            "fn main()->int:\n  node=Node {visit:visit}\n  node.visit(node)\n");
    if (!callable.ok())
      for (const auto &d : callable.diagnostics)
        std::cerr << renderDiagnostic(*callable.source, d);
    require(callable.ok(), "nominal callback signature expanded as stored field");
    require(bool(lowerRecordTypes(*callable.module, context)), "checked callback field lowering");
#ifndef KALANDRA_PHYSICAL_LAYOUT_FRONTEND_ONLY
    require(bool(lowerNative(*callable.module)), "opaque callback source failed native lowering");
#endif
    for (const auto &source :
         {"enum List:\n  Cons:\n    tail:List\nfn main()->int:\n  0\n",
          "type A:\n  child:B\ntype B:\n  child:A\nfn main()->int:\n  0\n",
          "type Bad:\n  next:Bad\n  unknown:Missing\nfn main()->int:\n  0\n"}) {
      auto result = analyze(source);
      require(!result.ok() && !result.module && !result.diagnostics.empty(),
              "recursive/invalid source published partial IR");
    }
    std::cout << "record physical layout passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
