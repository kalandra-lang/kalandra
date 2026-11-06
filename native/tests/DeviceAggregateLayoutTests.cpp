#include "../gpu-source/AggregateNativeLayout.h"
#include "../gpu-source/AggregateEntry.h"
#include "../gpu-source/AggregateGraphSchema.h"
#include "../gpu-source/ClosureNativeLayout.h"
#include "../gpu-source/ClosureGraphSchema.h"
#include "../runtime/DeviceGraphPack.h"
#include "../lib/RecordLLVMTypes.h"
#include "kalandra/Continuation.h"
#include "mlir/Target/LLVMIR/TypeToLLVM.h"
#include "llvm/IR/LLVMContext.h"
#include <array>
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

      auto closureModule = input;
      ValueType callback;
      callback.builtin = BuiltinValue::Function;
      auto signature = std::make_shared<FunctionSignature>();
      signature->parameters = {number()};
      signature->result = number();
      callback.callable = signature;
      const std::vector<ValueType> captures{boolean, number(NumericType::Int8), nominal(1), callback};
      TypedFunction closure;
      closure.name = "capturing";
      closure.captureCount = captures.size();
      TypedRecord expectedEnvironment;
      expectedEnvironment.name = "Environment";
      for (std::size_t i = 0; i < captures.size(); ++i) {
        closure.parameters.push_back(i);
        closureModule.symbols.push_back({"capture", captures[i], {}, 0, true});
        expectedEnvironment.fields.push_back({"capture", captures[i], {}});
      }
      closureModule.functions.push_back(closure);
      closureModule.functions.push_back(TypedFunction{});
      ClosureTransferPlan transfer;
      transfer.environments = {{0, closure.parameters}, {1, {}}};
      const auto nativeClosure = planNativeClosureLayouts(closureModule, transfer, llvm, layout);
      require(nativeClosure && nativeClosure->carrierBytes == 16 &&
                  nativeClosure->codeOffset == 0 && nativeClosure->environmentOffset == 8 &&
                  nativeClosure->environments.size() == 2 &&
                  nativeClosure->environments[1].bytes == 0 &&
                  nativeClosure->environments[1].captures.empty(),
              "native closure carrier or empty environment layout changed");
      // Independent synchronous CPU record lowering reproduces the literal
      // struct used by Backend::closureEnvironment, including descriptor fields.
      closureModule.records.push_back(expectedEnvironment);
      auto closureRecords = lowerRecordTypes(closureModule, mlir);
      require(bool(closureRecords), "independent closure environment type rejected");
      auto *expected = llvm::cast<llvm::StructType>(
          translator.translateType(closureRecords->back()));
      const auto &actualEnvironment = nativeClosure->environments[0];
      require(actualEnvironment.bytes == layout.getTypeAllocSize(expected).getFixedValue() &&
                  actualEnvironment.alignment == layout.getABITypeAlign(expected).value(),
              "closure environment native size/alignment disagrees with CPU lowering");
      for (std::size_t i = 0; i < captures.size(); ++i) {
        const auto &field = actualEnvironment.captures[i];
        require(field.symbol == i &&
                    field.offset == layout.getStructLayout(expected)->getElementOffset(i) &&
                    field.bytes == layout.getTypeAllocSize(expected->getElementType(i)).getFixedValue() &&
                    field.alignment == layout.getABITypeAlign(expected->getElementType(i)).value(),
                "closure capture field offset/size/alignment disagrees with CPU lowering");
      }
      auto malformed = transfer;
      std::swap(malformed.environments[0].captures[0], malformed.environments[0].captures[1]);
      require(!planNativeClosureLayouts(closureModule, malformed, llvm, layout) &&
                  !planNativeClosureLayouts(closureModule, transfer, llvm, layout, 1),
              "closure layout accepted reordered captures or ignored its work budget");
      malformed = transfer;
      malformed.environments.push_back(transfer.environments[0]);
      require(!planNativeClosureLayouts(closureModule, malformed, llvm, layout),
              "duplicate closure environment target accepted");
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
    std::size_t graphBudget = 300000;
    auto graph = projectDeviceAggregates(input, roots, graphBudget, &*storage);
    require(graph && graph->nodes.size() == 3 && graph->leaves.size() == 2 &&
                graph->nodes[1].discriminator && graph->nodes[2].boxed &&
                graph->nodes[2].activeTag == 2 && graph->nodes[2].deviceWord == 1 &&
                graph->nodes[2].children.empty(),
            "graph projection did not terminate at the guarded recursive box");
    auto graphSchema = planAggregateGraphSchema(input, roots, *nativeRecursive);
    require(graphSchema && graphSchema->layouts.size() == 1 &&
                graphSchema->roots == std::vector<std::size_t>{1},
            "recursive graph schema did not intern the boxed layout");
    const auto &rule = graphSchema->layouts[0].cells;
    require(rule.size() == 2 && rule[0].rule.minimum == 1 && rule[0].rule.maximum == 2 &&
                rule[1].rule.target == 1 && !rule[1].rule.nullable &&
                rule[1].guards.size() == 1 && rule[1].guards[0].tag == 2 &&
                rule[1].nativeOffset == nativeRecursive->at(0).fields[1].nativeOffset &&
                rule[1].nativeBytes == 8,
            "recursive graph schema lost typed edge or active enum tag");
    auto referenceRule = rule[1].rule;
    referenceRule.guards = rule[1].guards;
    const std::array graphCells{rule[0].rule, referenceRule};
    const std::array wireLayouts{runtime::DeviceGraphLayout{graphCells, true}};
    const std::vector<std::uint64_t> listWire{runtime::DeviceGraphWireABI::Magic, 2, 1,
                                              1, 1, 1, 1, 2, 2, 1, 0};
    require(runtime::validateDeviceGraph(listWire, wireLayouts, 1, 100, [] { return false; }) ==
                runtime::DeviceGraphValidation::Valid,
            "compiler-generated schema rejected a finite list");
    const std::array nativeCells{
        runtime::DeviceGraphNativeCell{rule[0].rule, rule[0].nativeOffset, rule[0].nativeBytes},
        runtime::DeviceGraphNativeCell{referenceRule, rule[1].nativeOffset, rule[1].nativeBytes}};
    const auto nativeStride = graphSchema->layouts[0].nativeStride;
    const std::array nativeLayouts{runtime::DeviceGraphNativeLayout{nativeCells, nativeStride, true}};
    std::vector<std::byte> first(nativeStride), last(nativeStride);
    const std::uint64_t nextTag = 2, endTag = 1;
    auto *tail = last.data();
    std::memcpy(first.data() + rule[0].nativeOffset, &nextTag, sizeof(nextTag));
    std::memcpy(last.data() + rule[0].nativeOffset, &endTag, sizeof(endTag));
    std::memcpy(first.data() + rule[1].nativeOffset, &tail, sizeof(tail));
    std::fill(last.begin() + rule[1].nativeOffset, last.end(), std::byte{255});
    unsigned reads = 0;
    const auto resolve = [&](std::size_t layout, std::span<const std::byte> field)
        -> std::expected<runtime::DeviceGraphNativeView, runtime::DeviceGraphValidation> {
      ++reads;
      const std::byte *pointer = nullptr;
      std::memcpy(&pointer, field.data(), sizeof(pointer));
      if (layout != 1 || pointer != tail)
        return std::unexpected(runtime::DeviceGraphValidation::Invalid);
      return runtime::DeviceGraphNativeView{1, 1, last};
    };
    auto packed = runtime::packDeviceGraph({1, 1, first}, nativeLayouts, 100, 100, resolve,
                                           [] { return false; });
    require(packed && *packed == listWire && reads == 1,
            "native list packing did not preserve its shape and skip the inactive reference");
    packed = runtime::packDeviceGraph({1, 1, first}, nativeLayouts, 8, 100, resolve,
                                      [] { return false; });
    require(!packed && packed.error() == runtime::DeviceGraphValidation::Budget,
            "native list packing exceeded its wire budget");
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
    const std::array graphRoots{nominal(0), nominal(1)};
    graph = projectDeviceAggregates(input, graphRoots, graphBudget, &*storage);
    require(graph && graph->nodes.size() == 6 && graph->leaves.size() == 3 &&
                deviceRecordArrayValue(graph->nodes[2].type) &&
                deviceRecordArrayValue(graph->nodes[5].type) &&
                graph->nodes[5].root == 1 && graph->nodes[5].deviceWord == 0,
            "graph projection did not retain record array identity and inline offset");
    nativeRecursive = planNativeAggregateLayouts(input, llvm, graphLayout);
    require(bool(nativeRecursive), "native graph array layout failed");
    graphSchema = planAggregateGraphSchema(input, roots, *nativeRecursive);
    require(graphSchema && graphSchema->layouts.size() == 2 &&
                graphSchema->layouts[0].cells[0].rule.target == 2 &&
                graphSchema->layouts[0].cells[0].rule.nullable &&
                graphSchema->layouts[0].cells[0].nativeBytes > 8 &&
                graphSchema->layouts[1].type.array &&
                graphSchema->layouts[1].cells[0].rule.target == 2 &&
                graphSchema->layouts[1].cells[1].rule.maximum == UINT64_MAX,
            "compiler graph schema lost the recursive array layout");
    require(!planAggregateGraphSchema(input, roots, *nativeRecursive, 1), "graph schema ignored work budget");
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
    auto closureSource = check(parse(std::make_shared<SourceFile>("closure-graph.k", R"k(
type Batch:
  run:fn(int)->int
  values:[int]
fn make()->Batch:
  values=[3,7]
  inner=fn(n:int):n+values[0]
  outer=fn(n:int):inner(n)+values[1]
  Batch {run:outer,values:values}
fn main()->int:
  batch=make()
  batch.run(1)
)k")));
    require(closureSource.ok(), "closure graph source rejected");
    const auto &closureModule = *closureSource.module;
    const auto closureRoot = closureModule.functions[0].result;
    const auto closurePlan = planClosureTransfer(closureModule, std::span(&closureRoot, 1));
    require(bool(closurePlan), "closure graph target planning failed");
    const auto closureABI = planNativeClosureLayouts(closureModule, *closurePlan, llvm, graphLayout);
    const auto closureRecords = planNativeAggregateLayouts(closureModule, llvm, graphLayout);
    require(closureABI && closureRecords, "closure graph native ABI planning failed");
    const auto &closureType = closureModule.functions[0].resultType;
    const auto closureGraph = planClosureGraphSchema(closureModule, std::span(&closureType, 1),
                                                     *closurePlan, *closureABI, *closureRecords);
    if (!closureGraph)
      throw std::runtime_error(closureGraph.error());
    require(closureGraph->imports.size() == 2, "nested closure graph lost a target");
    require(std::ranges::count_if(closureGraph->graph.layouts, [](const auto &layout) {
              return layout.type.array;
            }) == 1, "array shape refinement split a shared capture layout");
    require(std::ranges::count(closureGraph->kinds, ClosureGraphSchema::Kind::Callable) == 1,
            "nested closures with the same signature split their carrier layout");
    for (const auto &import : closureGraph->imports) {
      require(import.environmentLayout && import.environmentWord && import.carrierWords == 3,
              "nested closure environment schema missing");
    }
    for (std::size_t limit : {0u, 1u, 8u})
      require(!planClosureGraphSchema(closureModule, std::span(&closureType, 1),
                                     *closurePlan, *closureABI, *closureRecords, limit),
              "partial closure graph escaped the work budget");
    std::cout
        << "device/native aggregate schemas: padding, all numeric/Bool arrays, nested aliases, "
           "enum activity, budgets and iterative layout passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
