#include "../gpu-source/AggregateNativeLayout.h"
#include "../gpu-source/AggregateEntry.h"
#include "../lib/RecordLLVMTypes.h"
#include "kalandra/Continuation.h"
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/LLVMContext.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static ValueType number(NumericType element = NumericType::Int64, bool array = false) {
  ValueType type;
  type.element = element;
  type.array = type.tensor = array;
  if (array)
    type.shape = {{std::nullopt, 1}};
  return type;
}
static ValueType nominal(std::size_t id) {
  ValueType type;
  type.builtin = BuiltinValue::Record;
  type.record = id;
  return type;
}
static TypedRecord record(std::initializer_list<ValueType> fields) {
  TypedRecord result;
  result.name = "Record";
  for (auto &type : fields)
    result.fields.push_back({"field", type, {}});
  return result;
}
int main() {
  try {
    ValueType boolean;
    boolean.builtin = BuiltinValue::Bool;
    auto boolArray = boolean;
    boolArray.array = boolArray.tensor = true;
    boolArray.shape = {{std::nullopt, 1}};
    TypedModule input;
    // Deliberately put child declarations after users and repeat a child. Array
    // descriptors are forty native bytes but exactly one device handle cell.
    input.records = {record({boolean, nominal(1), nominal(1), boolArray}),
                     record({number(NumericType::Int8), number(NumericType::Float64),
                             number(NumericType::UInt64, true)})};
    std::vector<ValueType> roots{nominal(0)};
    auto device = planDeviceAggregateLayouts(input, roots);
    require(device && device->size() == 2, "reachable aggregate schema missing");
    require(device->at(0).words == 8 &&
                device->at(0).fields == std::vector<std::size_t>{0, 1, 4, 7} &&
                device->at(0).owned == std::vector<std::size_t>{3, 6, 7},
            "nested/repeated owned leaf projection changed");
    // Compare against the synchronous CPU type emitter independently of the
    // task emitter used by the boundary planner, for each supported ABI family.
    mlir::MLIRContext mlir;
    mlir.loadDialect<mlir::LLVM::LLVMDialect>();
    llvm::LLVMContext llvm;
    auto ordinary = lowerRecordTypes(input, mlir);
    require(bool(ordinary), "CPU schema rejected");
    mlir::LLVM::TypeToLLVMIRTranslator translator(llvm);
    for (auto spelling :
         {"e-p:64:64-i64:64-i128:128-n32:64-S128",
          "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128",
          "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"}) {
      llvm::DataLayout layout(spelling);
      auto abi = planAggregateABI(input, llvm, layout, roots);
      require(abi && abi->size() == 2, "aggregate ABI rejected");
      for (std::size_t id = 0; id < input.records.size(); ++id) {
        auto *type = llvm::cast<llvm::StructType>(translator.translateType((*ordinary)[id]));
        const auto &actual = abi->at(id);
        require(actual.nativeBytes == layout.getTypeAllocSize(type) &&
                    actual.nativeAlignment == layout.getABITypeAlign(type).value() &&
                    actual.deviceWords == device->at(id).words,
                "native and device aggregate size mixed");
        for (std::size_t field = 0; field < actual.fields.size(); ++field) {
          const auto &entry = actual.fields[field];
          require(entry.nativeOffset == layout.getStructLayout(type)->getElementOffset(field) &&
                      entry.nativeBytes == layout.getTypeAllocSize(type->getElementType(field)) &&
                      entry.nativeAlignment ==
                          layout.getABITypeAlign(type->getElementType(field)).value() &&
                      entry.deviceOffset == device->at(id).fields[field],
                  "native padding/field correspondence mismatch");
        }
      }
      require(abi->at(1).fields[2].nativeBytes == 40 && abi->at(1).fields[2].deviceWords == 1 &&
                  abi->at(1).fields[2].storage == RecordFieldStorage::CollectionDescriptor,
              "array descriptor mistaken for wire payload");
      require(abi->at(0).fields[0].nativeBytes == 1 && abi->at(0).fields[0].deviceWords == 1,
              "Bool native width confused with canonical cell width");
    }
    // Payload activity is scoped to its containing enum; a nested enum adds its
    // own tag check rather than replacing the parent's guard.
    input.records.push_back(record({number(), nominal(0), number(NumericType::Int32)}));
    input.records[2].variants = {{"None", {}, {}}, {"Batch", {}, {1}}, {"Count", {}, {2}}};
    roots = {nominal(2)};
    device = planDeviceAggregateLayouts(input, roots);
    require(device && device->at(2).fieldTags == std::vector<std::size_t>{0, 2, 3} &&
                device->at(2).owned == std::vector<std::size_t>{4, 7, 8},
            "enum active-tag/owned projection mismatch");
    input.records[2].variants[2].fields = {1};
    require(!planDeviceAggregateLayouts(input, roots), "duplicate enum payload admitted");
    input.records[2].variants[2].fields = {2};
    for (const auto &description : numericTypes) {
      input.records = {record({number(description.type), number(description.type, true)})};
      roots = {nominal(0)};
      device = planDeviceAggregateLayouts(input, roots);
      require(device && device->at(0).words == 2 &&
                  device->at(0).owned == std::vector<std::size_t>{1},
              "numeric canonical width depends on native element width");
    }
    require(!planDeviceAggregateLayouts(input, roots, 1), "layout work budget ignored");
    input.records = {record({number(), nominal(0)})};
    input.records[0].variants = {{"End", {}, {}}, {"Next", {}, {1}}};
    require(!planDeviceAggregateLayouts(input, roots), "recursive graph infinitely flattened");
    auto nativeRecursive =
        planNativeAggregateLayouts(input, llvm, llvm::DataLayout("e-p:64:64-i64:64"));
    require(nativeRecursive && nativeRecursive->at(0).fields[1].nativeBytes == 8 &&
                nativeRecursive->at(0).fields[1].storage == RecordFieldStorage::ManagedReference,
            "native recursive slot incorrectly depends on device inline admission");
    auto storage = planRecordStorage(input);
    require(bool(storage), "recursive record storage planning failed");
    device = planDeviceAggregateLayouts(input, roots, 300000, &*storage);
    require(device && device->at(0).words == 2 && device->at(0).boxed[1] &&
                device->at(0).owned == std::vector<std::size_t>{1},
            "recursive enum edge did not terminate at an owned handle");
    const llvm::DataLayout graphLayout("e-p:64:64-i64:64");
    auto graphABI = planAggregateABI(input, llvm, graphLayout, roots, 300000, &*storage);
    require(graphABI && graphABI->at(0).fields[1].deviceWords == 1 &&
                graphABI->at(0).fields[1].deviceOffset == 1 &&
                graphABI->at(0).fields[1].nativeBytes == 8 &&
                graphABI->at(0).fields[1].storage == RecordFieldStorage::ManagedReference,
            "paired graph ABI expanded a managed recursive link");
    auto recordArray = nominal(1);
    recordArray.array = recordArray.tensor = true;
    recordArray.shape = {{std::nullopt, 1}};
    input.records = {record({recordArray}), record({nominal(0), number()})};
    storage = planRecordStorage(input);
    require(bool(storage), "recursive array storage planning failed");
    device = planDeviceAggregateLayouts(input, roots, 300000, &*storage);
    require(device && device->size() == 2 && device->at(0).words == 1 &&
                device->at(0).owned == std::vector<std::size_t>{0} &&
                device->at(1).words == 2 && device->at(1).owned == std::vector<std::size_t>{0} &&
                !device->at(0).boxed[0] && !device->at(1).boxed[0],
            "record array handle did not break inline expansion");
    graphABI = planAggregateABI(input, llvm, graphLayout, roots, 300000, &*storage);
    require(graphABI && graphABI->at(0).fields[0].deviceWords == 1 &&
                graphABI->at(0).fields[0].nativeBytes > 8 &&
                graphABI->at(0).fields[0].storage == RecordFieldStorage::CollectionDescriptor &&
                graphABI->at(1).fields[0].storage == RecordFieldStorage::Inline,
            "paired graph ABI confused native record array backing and canonical handles");
    require(!planDeviceAggregateLayouts(input, roots),
            "flat boundary accepted recursive record array");
    roots = {nominal(100)};
    require(!planDeviceAggregateLayouts(input, roots), "invalid nominal identity admitted");
    // An iterative deep layout, bounded by work rather than C++ recursion.
    constexpr std::size_t count = 2000;
    input.records.clear();
    for (std::size_t i = 0; i < count; ++i)
      input.records.push_back(i + 1 == count ? record({number()}) : record({nominal(i + 1)}));
    roots = {nominal(0)};
    device = planDeviceAggregateLayouts(input, roots);
    require(device && device->size() == count && device->at(0).words == 1,
            "deep acyclic schema used compiler recursion");
    input.symbols.push_back({"value", nominal(0), {}, 0, true});
    input.expressions.push_back({{}, number(), Int64Literal{0}});
    TypedFunction function;
    function.name = "work";
    function.parameters = {0};
    function.resultType = number();
    function.result = 0;
    input.functions.push_back(function);
    auto normalized = normalizeDeviceAggregateEntry(input, 0);
    require(normalized && normalized->plan.nodes.size() == count + 1 &&
                normalized->plan.leaves.size() == 1 && normalized->module.functions.size() == 2 &&
                input.functions.size() == 1,
            "deep entry normalization recursed or modified its source module");
    require(!normalizeDeviceAggregateEntry(input, 0, 4), "entry work budget ignored");
    auto checked = check(parse(std::make_shared<SourceFile>("aggregate-entry.k", R"(
enum Inner:
  Empty
  Data:
    values:[int]
enum Outer:
  Empty
  Data:
    inner:Inner
fn work(value:Outer)->int:
  match value:
    Outer.Empty:0
    Outer.Data {inner}:
      match inner:
        Inner.Empty:1
        Inner.Data {values}:values[0]
fn produce()->Outer:
  Outer.Data {inner:Inner.Data {values:[1,2]}}
)")));
    require(checked.ok(), "nested enum entry fixture rejected");
    normalized = normalizeDeviceAggregateEntry(*checked.module, 0);
    require(normalized && normalized->plan.leaves.size() == 3, "nested enum entry leaves lost");
    const auto arrayNode = normalized->plan.leaves.back();
    const auto &arrayLeaf = normalized->plan.nodes[arrayNode];
    require(arrayLeaf.type.array && arrayLeaf.activeTag == 2 &&
                arrayLeaf.parent != DeviceAggregateOccurrence::none &&
                normalized->plan.nodes[arrayLeaf.parent].activeTag == 2 &&
                normalized->plan.nodes[arrayLeaf.parent].parent == normalized->plan.roots[0],
            "inner enum activity replaced the enclosing enum guard");
    std::size_t resultBudget = 16;
    const auto &resultType = checked.module->functions.at(1).resultType;
    auto result = projectDeviceAggregates(*checked.module, std::span(&resultType, 1), resultBudget);
    require(result && result->leaves.size() == 3, "result projection lost nested fields");
    const auto &outerTag = result->nodes[result->leaves[0]];
    const auto &innerTag = result->nodes[result->leaves[1]];
    const auto &arrayResult = result->nodes[result->leaves[2]];
    require(outerTag.discriminator && outerTag.deviceWord == 0 &&
                innerTag.discriminator && innerTag.deviceWord == 1 &&
                arrayResult.type.array && arrayResult.deviceWord == 2 &&
                result->nodes[innerTag.parent].activeTag == 2 && arrayResult.activeTag == 2,
            "result projection lost device offsets or enum activity");
    resultBudget = 4;
    require(!projectDeviceAggregates(*checked.module, std::span(&resultType, 1), resultBudget),
            "result projection work budget ignored");
    const auto &wrapper = normalized->module.functions.at(normalized->function);
    const auto *call = std::get_if<FunctionCall>(&normalized->module.expressions[wrapper.result].operation);
    require(call && call->function == 0 && call->arguments.size() == 1 &&
                wrapper.parameters.size() == 3,
            "entry wrapper replaced source computation instead of calling it");
    auto planned = continuation::plan(normalized->module,
                                     {.entry = normalized->function,
                                      .nativeSubgraphs = false,
                                      .valueScopes = true,
                                      .inlineLoopCalls = true,
                                      .deviceValues = true});
    require(bool(planned), "normalized entry violates checked continuation invariants");
    const auto &instructions = planned->plans.at(normalized->function)->code;
    require(std::ranges::any_of(instructions, [&](const auto &op) {
              return op.code == continuation::Code::Kernel &&
                     op.source < checked.module->expressions.size() &&
                     std::holds_alternative<ArrayIndex>(checked.module->expressions[op.source].operation);
            }),
            "normalized wrapper omitted the original enum computation");
    std::cout
        << "device/native aggregate schemas: padding, all numeric/Bool arrays, nested aliases, "
           "enum activity, budgets and iterative layout passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
