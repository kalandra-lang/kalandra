#include "../gpu-source/AggregateNativeLayout.h"
#include "../gpu-source/AggregateEntry.h"
#include "../gpu-source/AggregateGraphSchema.h"
#include "../gpu-source/PointerBridge.h"
#include "../gpu-source/ClosureGraphSchema.h"
#include "../gpu-source/DeviceErrorLayout.h"
#include "../gpu-source/DeviceErrorEntry.h"
#include "../gpu-source/Supervisor.h"
#include "../lib/NativeLayout.h"
#include "../lib/TaskValueCopy.h"
#include "../metal/ElementwiseLayout.h"
#include "MatmulShader.h"
#include "DeviceHeap.h"
#include "MatmulTiles.h"
#include "NumericProgram.h"
#include "NumericShader.h"
#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include "kalandra/MetalSource.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/Semantics.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
using namespace kalandra;
namespace {
struct Rejected {
  Diagnostic diagnostic;
};
struct Value {
  enum Kind {
    Tensor,
    Target,
    Task,
    Scalar,
    Boolean,
    Error,
    FaultKind,
    Text,
    Array,
    Aggregate,
    Callable
  } kind;
  std::size_t slot = 0;
  bool gpu = false;
  std::uint32_t device = 0;
  std::optional<std::uint64_t> length = {};
  bool scalarTask = false;
  unsigned rank = 1;
  std::optional<std::uint64_t> width = {};
  std::optional<std::size_t> failure = {};
  std::array<std::optional<std::uint64_t>, 6> higherExtents{};
  NumericType element = NumericType::Float32;
  bool array = false, booleanArray = false, booleanTask = false;
  std::optional<std::size_t> record = {};
  std::optional<std::size_t> callable = {};
  std::optional<std::size_t> outcomeRecord = {};
  std::optional<FunctionId> outcomeHelper = {};
  std::optional<std::pair<std::size_t, std::size_t>> applicationError = {};
};
class Compiler {
  ArithmeticMode arithmetic_;
  GpuBackend backend_;
  bool testCpuExports_;
  Module syntax;
  std::map<std::string, std::size_t> cpuFunctions;
  TypedModule cpu;
  TypedModule placement;
  std::vector<std::string> cpuExports;
  std::set<FunctionId> usedCpuFunctions, usedContinuationCpu;
  const TypedModule *continuationCpu = nullptr;
  std::map<ValueId, FunctionId> continuationKernels;
  std::map<FunctionId, FunctionId> continuationImports;
  struct ClosureBridgeRequest { std::string name; std::vector<FunctionId> targets; };
  std::vector<ClosureBridgeRequest> closureBridges;
  std::set<FunctionId> closureCodeGetters;
  std::map<std::size_t, Value> arrayFields;
  std::string declarations, cpuEntries, aggregateDeclarations;
  std::map<std::size_t, NativeAggregateABI> aggregateTypes;
  std::vector<ValueType> callableTypes;
  std::optional<std::vector<NativeAggregateABI>> nativeAggregateLayouts;
  std::set<std::pair<bool, FunctionId>> declaredAggregateFunctions;
  std::set<std::pair<const TypedModule *, std::size_t>> checkedAggregateSchemas;
  std::size_t cpuEntryCount = 0;
  std::string activeGroup;
  std::optional<std::size_t> admissionExit;
  bool conditionalHost = false;
  static std::string jump(std::size_t instruction) {
    return "f.pc=@pc" + std::to_string(instruction) + "@; continue;\n";
  }
  static std::string failureRef(const Value &value) {
    return "f.v" + std::to_string(*value.failure);
  }
  void completed(const Value &value, SourceSpan span) const {
    if (value.failure)
      reject(span, "expected error result requires try or catch before use");
  }
  std::string admitted(const std::string &handle) {
    std::string code = "if (!(" + handle + ")) { auto failure=task.failure(" + activeGroup +
                       "); if(failure.kind!=TaskFailure::None){\n";
    // Another worker can fail and settle the group between argument evaluation
    // and admission. Preserve that error instead of faulting on an empty
    // handle.
    if (admissionExit)
      code += jump(*admissionExit);
    else
      code += "task.fail(failure); return;\n";
    return code + "} " + error("m::Error::Invalid") + "}\n";
  }
  // Admit the placed function's typed closure, not CPU expressions used to
  // compute its arguments. Host preflight checks fixed and inferred shape
  // contracts.
  void admitGpuTypes(FunctionId root, const TypedModule *source = nullptr) const {
    const auto &cpu = source ? *source : this->cpu;
    std::set<FunctionId> visited;
    std::vector<FunctionId> pending{root};
    std::set<std::size_t> errorFamilies;
    while (!pending.empty()) {
      auto function = pending.back();
      pending.pop_back();
      if (!visited.insert(function).second)
        continue;
      const auto &fn = cpu.functions[function];
      const auto supported = [&](const ValueType &type) {
        if (type.builtin == BuiltinValue::Numeric &&
            !numericArithmeticAvailable(type.element, NumericTarget::AppleGpu))
          reject(fn.declaration, "GPU kernel numeric type is not implemented");
        if (type.fallible && type.errorRecord)
          errorFamilies.insert(*type.errorRecord);
      };
      supported(fn.resultType);
      for (auto parameter : fn.parameters)
        supported(cpu.symbols[parameter].type);
      for (auto value : fn.expressions) {
        const auto &expression = cpu.expressions[value];
        if (expression.type.fallible && expression.type.errorRecord)
          errorFamilies.insert(*expression.type.errorRecord);
        const auto &op = expression.operation;
        if (auto *call = std::get_if<FunctionCall>(&op))
          pending.push_back(call->function);
        if (auto *map = std::get_if<ArrayMap>(&op))
          pending.push_back(map->function);
        if (auto *reference = std::get_if<FunctionReference>(&op))
          pending.push_back(reference->function);
        if (arithmetic_ != ArithmeticMode::Hardware)
          continue;
        bool requiresDouble =
            expression.type.element == NumericType::Float64 &&
            (std::holds_alternative<ScalarBinary>(op) || std::holds_alternative<TensorBinary>(op) ||
             std::holds_alternative<ScalarUnary>(op) || std::holds_alternative<TensorUnary>(op) ||
             std::holds_alternative<Matmul>(op) || std::holds_alternative<ReduceSum>(op));
        if (auto *unary = std::get_if<TensorUnary>(&op);
            unary && unary->operation == TokenKind::Relu)
          requiresDouble = false; // ReLU selects integer bits; no binary64 arithmetic.
        if (auto *cast = std::get_if<NumericConvert>(&op))
          requiresDouble |= expression.type.element == NumericType::Float64 ||
                            cpu.expressions[cast->input].type.element == NumericType::Float64;
        if (auto *compare = std::get_if<ScalarCompare>(&op))
          requiresDouble |= cpu.expressions[compare->left].type.element == NumericType::Float64;
        if (requiresDouble && backend_ == GpuBackend::Metal)
          reject(expression.span, "Metal has no hardware float64 arithmetic; explicitly select "
                                  "--arithmetic hardware-with-software-fallback or software");
      }
    }
    if (!errorFamilies.empty()) {
      const std::vector families(errorFamilies.begin(), errorFamilies.end());
      auto layouts = planDeviceErrorPayloadLayouts(cpu, families);
      if (!layouts)
        reject(cpu.functions[root].declaration, layouts.error());
    }
  }

  std::map<std::size_t, std::size_t> entries;
  std::map<FunctionId, NumericShader> numericShaders;
  std::map<FunctionId, NumericProgram> numericPrograms;
  std::map<FunctionId, FunctionId> deviceErrorEntries;
  std::map<FunctionId, DeviceAggregateProjection> aggregateEntries;
  std::size_t aggregateEntryWorkRemaining = 300000;
  std::vector<std::string> steps, fields;
  std::size_t next = 0;
  struct Depth {
    std::size_t &value;
    explicit Depth(std::size_t &v) : value(v) { ++value; }
    ~Depth() { --value; }
  };
  [[noreturn]] void reject(SourceSpan span, std::string message) const {
    throw Rejected{{"G001", std::move(message), span}};
  }
  std::string text(SourceSpan span) const { return std::string(syntax.source->slice(span)); }
  const TypedExpr &node(ValueId id) const { return placement.expressions.at(id); }
  FunctionId cpuFunction(FunctionId id, SourceSpan span) const {
    auto found = cpuFunctions.find(placement.functions.at(id).name);
    if (found == cpuFunctions.end())
      reject(span, "task/host entry requires an ordinary CPU function");
    return found->second;
  }
  std::string field(std::string type) {
    if (next >= 20000)
      reject({}, "Metal continuation value limit exceeded");
    auto name = "v" + std::to_string(next++);
    fields.push_back(type + " " + name + "{};");
    return name;
  }
  static std::string ref(const Value &v) { return "f.v" + std::to_string(v.slot); }
  static std::string elementName(NumericType element) {
    constexpr std::array names{"Int8",   "Int16",  "Int32",  "Int64",   "UInt8",
                               "UInt16", "UInt32", "UInt64", "Float32", "Float64"};
    return "kalandra::NumericType::" + std::string(names.at(static_cast<unsigned>(element)));
  }
  static std::string scalarType(NumericType element) {
    return "NumericValue<" + elementName(element) + ">";
  }
  static std::string tensorDescriptor(NumericType element, unsigned rank) {
    return "CpuTensor<" + elementName(element) + "," + std::to_string(rank) + ">";
  }
  static std::string recordName(std::size_t id) { return "NativeRecord" + std::to_string(id); }
  static std::string recordLayoutName(std::size_t id) {
    return "kalandra_gpu_record_layout_" + std::to_string(id);
  }
  void validateRecordSchema(const TypedModule &source, std::size_t root) {
    std::vector<std::size_t> pending{root};
    std::size_t work = 0;
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (checkedAggregateSchemas.contains({&source, id}))
        continue;
      if (++work > 300000 || id >= source.records.size() || id >= placement.records.size())
        reject({}, "native aggregate projections require checked common nominal identities");
      const auto &a = source.records[id], &b = placement.records[id];
      bool same = a.name == b.name && a.fields.size() == b.fields.size() &&
                  a.variants.size() == b.variants.size() && a.error == b.error &&
                  a.taskErrorFamily == b.taskErrorFamily && a.errorMembers == b.errorMembers;
      for (std::size_t n = 0; same && n < a.fields.size(); ++n) {
        const auto &x = a.fields[n].type, &y = b.fields[n].type;
        same = a.fields[n].name == b.fields[n].name && x.builtin == y.builtin &&
               x.record == y.record && x.errorRecord == y.errorRecord && x.tensor == y.tensor &&
               x.array == y.array && x.task == y.task && x.fallible == y.fallible &&
               x.device == y.device && x.shape.size() == y.shape.size() &&
               (x.builtin != BuiltinValue::Numeric || x.element == y.element) &&
               (x.builtin != BuiltinValue::Function || sameFunctionValueType(x, y));
        for (std::size_t axis = 0; same && axis < x.shape.size(); ++axis)
          same = x.shape[axis].constant == y.shape[axis].constant;
        if (x.record)
          pending.push_back(*x.record);
      }
      for (std::size_t n = 0; same && n < a.variants.size(); ++n)
        same = a.variants[n].name == b.variants[n].name &&
               a.variants[n].fields == b.variants[n].fields;
      if (!same)
        reject({}, "native aggregate projections disagree on nominal field or enum schema");
      checkedAggregateSchemas.emplace(&source, id);
    }
  }
  std::string recordType(std::size_t id) {
    if (!aggregateTypes.contains(id)) {
      if (!nativeAggregateLayouts) {
        llvm::LLVMContext context;
        auto plan =
            planNativeAggregateLayouts(placement, context, llvm::DataLayout(nativeCpuDataLayout));
        if (!plan)
          reject({}, "native aggregate host boundary: " + plan.error());
        nativeAggregateLayouts = std::move(*plan);
      }
      const auto &abi = nativeAggregateLayouts->at(id);
      aggregateDeclarations += "struct alignas(" + std::to_string(abi.nativeAlignment) + ") " +
                               recordName(id) + " {std::array<std::byte," +
                               std::to_string(abi.nativeBytes) +
                               "> data{};};\nstatic_assert(sizeof(" + recordName(id) +
                               ")==" + std::to_string(abi.nativeBytes) +
                               ");\nextern \"C\" const ValueLayout " + recordLayoutName(id) + ";\n";
      aggregateTypes.emplace(id, abi);
    }
    return recordName(id);
  }
  static std::string callableName(std::size_t id) { return "NativeCallable" + std::to_string(id); }
  static std::string callableLayoutName(std::size_t id) {
    return "kalandra_gpu_callable_layout_" + std::to_string(id);
  }
  std::size_t callableType(const ValueType &type) {
    auto payload = type;
    payload.task = false;
    payload.fallible = false;
    for (std::size_t id = 0; id < callableTypes.size(); ++id)
      if (sameFunctionValueType(callableTypes[id], payload))
        return id;
    const auto id = callableTypes.size();
    callableTypes.push_back(payload);
    aggregateDeclarations += "struct " + callableName(id) +
                             " {const void *code=nullptr;const void *environment=nullptr;};\n"
                             "extern \"C\" const ValueLayout " +
                             callableLayoutName(id) + ";\n";
    return id;
  }
  static std::string valueStorage(const Value &value) {
    return value.callable && value.kind != Value::Task ? callableName(*value.callable)
           : value.record && value.kind != Value::Task ? recordName(*value.record)
           : value.array && value.kind != Value::Task  ? tensorDescriptor(value.element, 1)
           : value.kind == Value::Tensor               ? "m::Buffer"
           : value.kind == Value::Boolean              ? "bool"
                                                       : scalarType(value.element);
  }
  static std::string arrayLayout(const Value &value) {
    if (value.callable)
      return callableLayoutName(*value.callable);
    if (value.record)
      return recordLayoutName(*value.record);
    return "hostArrayLayout<" + elementName(value.element) + "," +
           (value.booleanArray ? "true" : "false") + ">()";
  }
  std::string arrayLayout(const ValueType &type) const {
    if (type.builtin == BuiltinValue::Function) {
      auto payload = type;
      payload.task = false;
      payload.fallible = false;
      for (std::size_t id = 0; id < callableTypes.size(); ++id)
        if (sameFunctionValueType(callableTypes[id], payload))
          return callableLayoutName(id);
      reject({}, "missing native callable ownership layout");
    }
    Value value{Value::Array};
    value.record = type.record;
    value.booleanArray = isBoolArray(type);
    value.element = value.booleanArray ? NumericType::UInt8 : type.element;
    return arrayLayout(value);
  }
  void trackArray(const Value &value) {
    if (value.kind == Value::Array || value.kind == Value::Aggregate ||
        value.kind == Value::Callable)
      arrayFields.emplace(value.slot, value);
  }
  std::string commitArray(const Value &value) const {
    if (value.kind != Value::Array && value.kind != Value::Aggregate &&
        value.kind != Value::Callable)
      return {};
    const auto found = arrayFields.find(value.slot);
    if (found == arrayFields.end())
      reject({}, "missing host array ownership slot");
    const auto index = std::to_string(std::distance(arrayFields.begin(), found));
    const auto commit = "if(!f.arrayScopes.commit(" + index + ",&" + ref(value) +
                        ")){hostArrayFailure(task);return;}\n";
    if ((value.record || value.callable) && value.failure)
      return "if(" + failureRef(value) + ".kind!=TaskFailure::None){" + ref(value) +
             "={};if(!f.arrayScopes.discard(" + index + ")){hostArrayFailure(task);return;}}else{" +
             commit + "}\n";
    std::string code;
    if (value.failure)
      code += "if(" + failureRef(value) + ".kind!=TaskFailure::None) " + ref(value) + "={};\n";
    return code + commit;
  }
  std::string arrayScopeDeclaration() const {
    if (arrayFields.empty())
      return {};
    std::string code = "inline const std::array<const ValueLayout*," +
                       std::to_string(arrayFields.size()) + "> arrayLayouts{{";
    for (const auto &[slot, value] : arrayFields) {
      (void)slot;
      code += "&" + arrayLayout(value) + ",";
    }
    return code + "}};\n";
  }
  std::optional<std::uint64_t> literalBits(ValueId id) const {
    const auto &expression = node(id);
    const auto bits = describeNumeric(expression.type.element)->bits;
    const auto mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    if (const auto *unary = std::get_if<ScalarUnary>(&expression.operation)) {
      auto input = literalBits(unary->operand);
      if (!input)
        return {};
      if (unary->operation == TokenKind::Plus)
        return input;
      if (unary->operation != TokenKind::Minus)
        return {};
      return isInteger(expression.type.element) ? (UINT64_C(0) - *input) & mask
                                                : *input ^ (UINT64_C(1) << (bits - 1));
    }
    return std::visit(
        [&](const auto &literal) -> std::optional<std::uint64_t> {
          using T = std::decay_t<decltype(literal)>;
          if constexpr (std::is_same_v<T, Float32Literal> || std::is_same_v<T, Float64Literal>)
            return literal.bits;
          else if constexpr (std::is_same_v<T, Int32Literal> || std::is_same_v<T, UInt32Literal> ||
                             std::is_same_v<T, Int64Literal> || std::is_same_v<T, UInt64Literal>)
            return static_cast<std::uint64_t>(literal.value) & mask;
          return {};
        },
        expression.operation);
  }
  static std::string scalarBits(NumericType element, std::uint64_t bits) {
    return "std::bit_cast<" + scalarType(element) + ">(std::uint" +
           std::to_string(describeNumeric(element)->bits) + "_t(" + std::to_string(bits) + "ull))";
  }
  std::string error(std::string expr) const {
    return "task.fault(std::int64_t(" + expr + ")); return;";
  }
  std::string check(std::string expr) const {
    return "if (!(" + expr + ")) { " + error("m::Error::Invalid") + " }\n";
  }
  std::string device(const Value &target) const {
    return "r.device(" + std::to_string(target.device) + ")";
  }
  static std::optional<std::uint64_t> extent(const Value &value, unsigned axis) {
    return axis == 0 ? value.length : axis == 1 ? value.width : value.higherExtents.at(axis - 2);
  }
  static void setExtent(Value &value, unsigned axis, std::optional<std::uint64_t> known) {
    if (axis == 0)
      value.length = known;
    else if (axis == 1)
      value.width = known;
    else
      value.higherExtents.at(axis - 2) = known;
  }
  std::optional<std::uint64_t> tensorType(const TypeSyntax &t, bool dynamicOnly = false) const {
    const auto numeric =
        t.element ? resolveNumeric(text(*t.element)) : std::optional<NumericType>{};
    if (!t.tensor || !numeric || t.dimensions.size() > 8)
      reject(t.span, "the Metal source bridge requires rank-zero through "
                     "rank-eight numeric tensors");
    const auto bytes = describeNumeric(*numeric)->bytes();
    std::uint64_t count = 1;
    bool allKnown = true, empty = false;
    for (const auto &d : t.dimensions) {
      if (dynamicOnly && d.extent)
        reject(t.span, "GPU tensor parameters/results currently require dynamic extents");
      if (d.extent && *d.extent > std::uint64_t(INT64_MAX) / bytes)
        reject(t.span, "tensor extent exceeds the checked allocation range");
      allKnown &= d.extent.has_value();
      empty |= d.extent && *d.extent == 0;
    }
    if (allKnown && !empty)
      for (const auto &d : t.dimensions) {
        if (*d.extent > std::uint64_t(INT64_MAX) / bytes / count)
          reject(t.span, "tensor shape exceeds the checked allocation range");
        count *= *d.extent;
      }
    return t.dimensions.empty() ? std::optional<std::uint64_t>{1} : t.dimensions[0].extent;
  }
  std::optional<std::uint32_t> constantBits(ValueId id) const {
    if (const auto *literal = std::get_if<Float32Literal>(&node(id).operation))
      return literal->bits;
    if (const auto *unary = std::get_if<ScalarUnary>(&node(id).operation)) {
      auto value = constantBits(unary->operand);
      if (value && unary->operation == TokenKind::Minus)
        *value ^= 0x80000000u;
      return value;
    }
    return {};
  }
  std::string lengthGuard(const Value &value, std::uint64_t wanted, SourceSpan span,
                          unsigned axis = 0) const {
    auto known = extent(value, axis);
    if (known) {
      if (*known == wanted)
        return {};
      if (!conditionalHost)
        reject(span, "tensor shape mismatch");
    }
    auto [line, column] = syntax.source->location(span.begin);
    return "if (" + ref(value) + ".shape().size()!=" + std::to_string(value.rank) + " || " +
           ref(value) + ".shape()[" + std::to_string(axis) + "]!=" + std::to_string(wanted) +
           ") { task.fault(1," + std::to_string(line) + "," + std::to_string(column) +
           "); return; }\n";
  }
  void requireLength(const Value &value, std::uint64_t wanted, SourceSpan span, unsigned axis = 0) {
    if (auto guard = lengthGuard(value, wanted, span, axis); !guard.empty())
      steps.push_back(std::move(guard));
  }
  struct PrimitiveEntry {
    enum Kind { Input, Sum, Matrix, Scalar, MatrixSum } kind = Input;
    std::size_t left = 0, right = 0;
    bool mapped = false, post = false;
    bool leftMapped = false, rightMapped = false;
    bool scalarPost = false;
  };
  mutable std::map<FunctionId, std::optional<PrimitiveEntry>> primitiveEntries;
  std::optional<PrimitiveEntry> primitiveEntry(FunctionId root) const {
    if (auto found = primitiveEntries.find(root); found != primitiveEntries.end())
      return found->second;
    const auto &entry = cpu.functions.at(root);
    const bool sumSignature =
        !entry.resultType.tensor && entry.resultType.builtin == BuiltinValue::Numeric &&
        entry.resultType.element == NumericType::Float32 &&
        std::any_of(entry.parameters.begin(), entry.parameters.end(), [&](auto parameter) {
          const auto &type = cpu.symbols[parameter].type;
          return type.tensor && !type.shape.empty() && type.shape.size() <= 8;
        });
    const bool matrixSignature =
        entry.resultType.tensor && entry.resultType.shape.size() == 2 &&
        std::any_of(entry.parameters.begin(), entry.parameters.end(), [&](auto parameter) {
          const auto &type = cpu.symbols[parameter].type;
          return type.tensor && type.shape.size() == 2;
        });
    auto &cached = primitiveEntries[root];
    if (!sumSignature && !matrixSignature)
      return cached;
    std::size_t work = 0, primitives = 0;
    std::set<FunctionId> active;
    using Result = std::optional<PrimitiveEntry>;
    // Track the reduced value through Boolean predicates as well as arithmetic.
    // A choice between constants can still depend on the resident reduction.
    auto scalarResult = [](std::initializer_list<Result> inputs) -> Result {
      Result result = PrimitiveEntry{PrimitiveEntry::Scalar};
      for (const auto &input : inputs) {
        if (!input ||
            (input->kind != PrimitiveEntry::Scalar && input->kind != PrimitiveEntry::Sum &&
             input->kind != PrimitiveEntry::MatrixSum))
          return {};
        if (input->kind != PrimitiveEntry::Scalar)
          result = input;
      }
      if (result->kind == PrimitiveEntry::Sum)
        result->post = true;
      else if (result->kind == PrimitiveEntry::MatrixSum)
        result->scalarPost = true;
      return result;
    };
    std::function<Result(FunctionId, const std::vector<PrimitiveEntry> &)> invoke;
    invoke = [&](FunctionId id, const std::vector<PrimitiveEntry> &args) -> Result {
      if (++work > 300000 || active.size() >= 128)
        reject(entry.declaration, "GPU primitive forwarding analysis limit exceeded");
      if (!active.insert(id).second)
        return {};
      const auto &fn = cpu.functions.at(id);
      if (args.size() != fn.parameters.size())
        return {};
      std::unordered_map<SymbolId, PrimitiveEntry> symbols;
      std::unordered_map<ValueId, PrimitiveEntry> values;
      for (std::size_t i = 0; i < args.size(); ++i)
        symbols.emplace(fn.parameters[i], args[i]);
      std::function<Result(ValueId)> emit = [&](ValueId value) -> Result {
        if (++work > 300000)
          reject(entry.declaration, "GPU primitive forwarding analysis limit exceeded");
        if (auto found = values.find(value); found != values.end())
          return found->second;
        const auto &node = cpu.expressions.at(value);
        Result result;
        if (const auto *read = std::get_if<SymbolRead>(&node.operation)) {
          if (auto found = symbols.find(read->symbol); found != symbols.end())
            result = found->second;
        } else if (const auto *check = std::get_if<ShapeCheck>(&node.operation))
          result = emit(check->input);
        else if (const auto *sum = std::get_if<ReduceSum>(&node.operation)) {
          auto input = emit(sum->input);
          if (input && sum->order == ReductionOrder::BalancedAdjacent && sum->rowMajor) {
            if (input->kind == PrimitiveEntry::Input && ++primitives == 1)
              result = PrimitiveEntry{PrimitiveEntry::Sum, input->left, 0, input->mapped};
            else if (input->kind == PrimitiveEntry::Matrix && ++primitives == 2) {
              result = input;
              result->kind = PrimitiveEntry::MatrixSum;
            }
          }
        } else if (const auto *matrix = std::get_if<Matmul>(&node.operation)) {
          auto left = emit(matrix->left), right = emit(matrix->right);
          if (left && right && left->kind == PrimitiveEntry::Input &&
              right->kind == PrimitiveEntry::Input &&
              matrix->order == ReductionOrder::BalancedAdjacent &&
              matrix->separatelyRoundedProducts && ++primitives == 1)
            result = PrimitiveEntry{PrimitiveEntry::Matrix, left->left,   right->left, false, false,
                                    left->mapped,           right->mapped};
        } else if (const auto *compare = std::get_if<ScalarCompare>(&node.operation)) {
          auto a = emit(compare->left), b = emit(compare->right);
          result = scalarResult({a, b});
        } else if (const auto *branch = std::get_if<ConditionalOperation>(&node.operation)) {
          const bool tensor = node.type.tensor;
          if (!(tensor ? pureLane(branch->trueResult) && pureLane(branch->falseResult)
                       : pureScalar(branch->trueResult) && pureScalar(branch->falseResult)))
            return {};
          auto condition = emit(branch->condition);
          auto saved = symbols;
          auto arm = [&](const std::vector<TypedStatement> &body, ValueId id) -> Result {
            for (const auto &statement : body) {
              if (!(tensor ? pureLane(statement.value) : pureScalar(statement.value)))
                return {};
              auto value = emit(statement.value);
              if (!value)
                return {};
              if (statement.binding)
                symbols[*statement.binding] = *value;
            }
            return emit(id);
          };
          auto a = arm(branch->whenTrue, branch->trueResult);
          symbols = saved;
          auto b = arm(branch->whenFalse, branch->falseResult);
          symbols = std::move(saved);
          if (!tensor)
            result = scalarResult({condition, a, b});
          else if (condition && condition->kind == PrimitiveEntry::Scalar && a && b) {
            if (a->kind == PrimitiveEntry::Input && b->kind == PrimitiveEntry::Input) {
              result = a;
              result->mapped = true;
            } else if (tensor &&
                       (a->kind == PrimitiveEntry::Matrix || b->kind == PrimitiveEntry::Matrix) &&
                       (a->kind == PrimitiveEntry::Matrix || a->kind == PrimitiveEntry::Input) &&
                       (b->kind == PrimitiveEntry::Matrix || b->kind == PrimitiveEntry::Input)) {
              // The one matrix primitive was computed before this pure choice.
              // Its result is a lane input to the existing resident epilogue.
              result = a->kind == PrimitiveEntry::Matrix ? a : b;
              result->post = true;
            }
          }
        } else if ((sumSignature || matrixSignature) &&
                   (std::holds_alternative<Float32Literal>(node.operation) ||
                    std::holds_alternative<BoolLiteral>(node.operation))) {
          result = PrimitiveEntry{PrimitiveEntry::Scalar};
        } else if ((sumSignature || matrixSignature) &&
                   (std::holds_alternative<TensorBinary>(node.operation) ||
                    std::holds_alternative<ScalarBinary>(node.operation))) {
          const auto *tensor = std::get_if<TensorBinary>(&node.operation);
          const auto *scalar = std::get_if<ScalarBinary>(&node.operation);
          auto a = emit(tensor ? tensor->left : scalar->left);
          auto b = emit(tensor ? tensor->right : scalar->right);
          if (a && b && (a->kind == PrimitiveEntry::Input || a->kind == PrimitiveEntry::Scalar) &&
              (b->kind == PrimitiveEntry::Input || b->kind == PrimitiveEntry::Scalar)) {
            result = a->kind == PrimitiveEntry::Input ? a : b;
            if (result->kind == PrimitiveEntry::Input)
              result->mapped = true;
          } else if ((matrixSignature || sumSignature) && tensor && a && b &&
                     (a->kind == PrimitiveEntry::Matrix || b->kind == PrimitiveEntry::Matrix) &&
                     (a->kind == PrimitiveEntry::Matrix || a->kind == PrimitiveEntry::Input ||
                      a->kind == PrimitiveEntry::Scalar) &&
                     (b->kind == PrimitiveEntry::Matrix || b->kind == PrimitiveEntry::Input ||
                      b->kind == PrimitiveEntry::Scalar)) {
            result = a->kind == PrimitiveEntry::Matrix ? a : b;
            result->post = true;
          } else if (!tensor && a && b &&
                     (a->kind == PrimitiveEntry::Sum || a->kind == PrimitiveEntry::MatrixSum ||
                      a->kind == PrimitiveEntry::Scalar) &&
                     (b->kind == PrimitiveEntry::Sum || b->kind == PrimitiveEntry::MatrixSum ||
                      b->kind == PrimitiveEntry::Scalar)) {
            result = a->kind != PrimitiveEntry::Scalar ? a : b;
            if (result->kind == PrimitiveEntry::MatrixSum)
              result->scalarPost = true;
            else
              result->post = true;
          }
        } else if ((sumSignature || matrixSignature) &&
                   (std::holds_alternative<TensorUnary>(node.operation) ||
                    std::holds_alternative<ScalarUnary>(node.operation))) {
          const auto *tensor = std::get_if<TensorUnary>(&node.operation);
          const auto *scalar = std::get_if<ScalarUnary>(&node.operation);
          auto input = emit(tensor ? tensor->operand : scalar->operand);
          if (input &&
              (input->kind == PrimitiveEntry::Input || input->kind == PrimitiveEntry::Scalar)) {
            result = input;
            if (result->kind == PrimitiveEntry::Input)
              result->mapped = true;
          } else if ((matrixSignature || sumSignature) && tensor && input &&
                     input->kind == PrimitiveEntry::Matrix) {
            result = input;
            result->post = true;
          } else if (!tensor && input &&
                     (input->kind == PrimitiveEntry::Sum ||
                      input->kind == PrimitiveEntry::MatrixSum)) {
            result = input;
            if (result->kind == PrimitiveEntry::MatrixSum)
              result->scalarPost = true;
            else
              result->post = true;
          }
        } else if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
          std::vector<PrimitiveEntry> arguments;
          for (auto argument : call->arguments) {
            auto input = emit(argument);
            if (!input)
              return {};
            arguments.push_back(*input);
          }
          result = invoke(call->function, arguments);
        }
        if (result)
          values.emplace(value, *result);
        return result;
      };
      // Inspect every statement, including unused bindings. Do not discard an
      // extra reduction, allocation or unsupported operation to fit one launch.
      for (const auto &statement : fn.body) {
        auto value = emit(statement.value);
        if (!value)
          return {};
        if (statement.binding)
          symbols[*statement.binding] = *value;
      }
      auto result = emit(fn.result);
      active.erase(id);
      return result;
    };
    std::vector<PrimitiveEntry> arguments;
    std::size_t tensorIndex = 0;
    for (auto parameter : entry.parameters)
      arguments.push_back(cpu.symbols[parameter].type.tensor
                              ? PrimitiveEntry{PrimitiveEntry::Input, tensorIndex++, 0}
                              : PrimitiveEntry{PrimitiveEntry::Scalar});
    auto result = invoke(root, arguments);
    if (result &&
        ((primitives == 1 && ((sumSignature && result->kind == PrimitiveEntry::Sum) ||
                              (matrixSignature && result->kind == PrimitiveEntry::Matrix))) ||
         (primitives == 2 && sumSignature && result->kind == PrimitiveEntry::MatrixSum)))
      cached = result;
    return cached;
  }
  bool reduction(std::size_t function) const {
    auto entry = primitiveEntry(function);
    return entry &&
           (entry->kind == PrimitiveEntry::Sum || entry->kind == PrimitiveEntry::MatrixSum);
  }
  bool matrixFunction(std::size_t function) const {
    auto entry = primitiveEntry(function);
    return entry &&
           (entry->kind == PrimitiveEntry::Matrix || entry->kind == PrimitiveEntry::MatrixSum);
  }
  bool scalarControl(FunctionId root) const {
    std::set<FunctionId> visited;
    std::vector<FunctionId> pending{root};
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (!visited.insert(id).second)
        continue;
      const auto &fn = cpu.functions.at(id);
      for (auto value : fn.expressions) {
        const auto &node = cpu.expressions.at(value);
        if (std::holds_alternative<ConditionalOperation>(node.operation))
          return true;
        if (const auto *call = std::get_if<FunctionCall>(&node.operation))
          pending.push_back(call->function);
      }
    }
    return false;
  }
  // Branch arms may be evaluated speculatively only when every operation is
  // pure, scalar or a lane calculation on one source. Shape checks and dispatch
  // stay outside.
  mutable std::map<ValueId, bool> scalarPurity, lanePurity;
  bool pureBranch(ValueId root, bool lane) const {
    auto &cache = lane ? lanePurity : scalarPurity;
    if (auto found = cache.find(root); found != cache.end())
      return found->second;
    const auto invalid = [&] {
      cache[root] = false;
      return false;
    };
    std::set<ValueId> visited;
    std::set<FunctionId> functions;
    std::vector<ValueId> pending{root};
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (auto found = cache.find(id); found != cache.end()) {
        if (!found->second)
          return invalid();
        continue;
      }
      if (!visited.insert(id).second)
        continue;
      if (visited.size() > 300000)
        reject(cpu.expressions.at(root).span, "GPU scalar branch analysis limit exceeded");
      const auto &node = cpu.expressions.at(id);
      const auto &type = node.type;
      if ((!lane && type.tensor) ||
          (type.tensor && (type.shape.empty() || type.shape.size() > 8)) || type.task ||
          type.fallible ||
          (type.builtin != BuiltinValue::Bool &&
           (type.builtin != BuiltinValue::Numeric || type.element != NumericType::Float32)))
        return invalid();
      if (std::holds_alternative<SymbolRead>(node.operation) ||
          std::holds_alternative<Float32Literal>(node.operation) ||
          std::holds_alternative<BoolLiteral>(node.operation))
        continue;
      if (const auto *binary = std::get_if<ScalarBinary>(&node.operation)) {
        pending.push_back(binary->left);
        pending.push_back(binary->right);
      } else if (const auto *binary = std::get_if<TensorBinary>(&node.operation); binary && lane) {
        pending.push_back(binary->left);
        pending.push_back(binary->right);
      } else if (const auto *unary = std::get_if<TensorUnary>(&node.operation); unary && lane)
        pending.push_back(unary->operand);
      else if (const auto *compare = std::get_if<ScalarCompare>(&node.operation)) {
        pending.push_back(compare->left);
        pending.push_back(compare->right);
      } else if (const auto *unary = std::get_if<ScalarUnary>(&node.operation))
        pending.push_back(unary->operand);
      else if (const auto *branch = std::get_if<ConditionalOperation>(&node.operation)) {
        pending.insert(pending.end(), {branch->condition, branch->trueResult, branch->falseResult});
        for (const auto &statement : branch->whenTrue)
          pending.push_back(statement.value);
        for (const auto &statement : branch->whenFalse)
          pending.push_back(statement.value);
      } else if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
        pending.insert(pending.end(), call->arguments.begin(), call->arguments.end());
        if (functions.insert(call->function).second) {
          const auto &fn = cpu.functions.at(call->function);
          if (lane) {
            if (!fn.preconditions.empty())
              return invalid();
            auto fixed = [](const ValueType &t) {
              return t.tensor && std::any_of(t.shape.begin(), t.shape.end(),
                                             [](const auto &e) { return e.constant.has_value(); });
            };
            if (fixed(fn.resultType))
              return invalid();
            for (auto parameter : fn.parameters)
              if (fixed(cpu.symbols[parameter].type))
                return invalid();
          }
          pending.push_back(fn.result);
          for (const auto &statement : fn.body)
            pending.push_back(statement.value);
        }
      } else
        return invalid();
    }
    for (auto id : visited)
      cache[id] = true;
    return true;
  }
  bool pureScalar(ValueId root) const { return pureBranch(root, false); }
  bool pureLane(ValueId root) const { return pureBranch(root, true); }
  bool retainedChoiceBindings(FunctionId function, std::size_t reads) const {
    // Constant predicates can erase an arm's reads. Keep projected resource
    // indices stable in operand/result maps as well as ordinary lane entries.
    return reads > 1 && scalarControl(function);
  }
  struct MatrixGraph {
    std::size_t inputs = 0, matrixCount = 0;
    std::vector<bool> maps;
    std::vector<std::uint32_t> reductions;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> broadcasts;
    bool reduce = false, scalarPost = false, tensorResult = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> products;
    std::map<std::size_t, std::vector<std::uint32_t>> mapInputs;
  };
  mutable std::map<FunctionId, std::optional<MatrixGraph>> matrixGraphs;
  const std::optional<MatrixGraph> &matrixGraph(FunctionId root) const {
    if (auto found = matrixGraphs.find(root); found != matrixGraphs.end())
      return found->second;
    auto &cached = matrixGraphs[root];
    const auto &entry = cpu.functions.at(root);
    if (entry.resultType.tensor &&
        (entry.resultType.shape.empty() || entry.resultType.shape.size() > 8))
      return cached;
    MatrixGraph graph;
    using Result = std::optional<std::int64_t>; // -1: scalar input; -2: reduced value.
    std::vector<std::set<std::size_t>> dependencies;
    auto dependency = [&](std::int64_t value) {
      return value <= -2 ? dependencies.at(-value - 2) : std::set<std::size_t>{};
    };
    // Speculated arm arithmetic carries tensor sources and scalar dependencies,
    // not graph stages. Keep its IDs outside the uint32 graph-value namespace.
    struct Lane {
      std::set<std::int64_t> sources;
      std::set<std::size_t> reduced;
    };
    constexpr std::int64_t laneBase = std::int64_t(UINT32_MAX) + 1;
    std::vector<Lane> lanes;
    std::size_t laneDepth = 0;
    auto sources = [&](std::int64_t v) {
      return v >= laneBase ? lanes.at(v - laneBase).sources : std::set<std::int64_t>{v};
    };
    std::size_t work = 0;
    auto mergedSources = [&](std::int64_t a, std::int64_t b) {
      auto result = sources(a), other = sources(b);
      result.insert(other.begin(), other.end());
      work += result.size();
      if (work > 300000)
        reject(entry.declaration, "GPU map operand analysis limit exceeded");
      return result;
    };
    auto laneDependency = [&](std::int64_t v) {
      return v >= laneBase ? lanes.at(v - laneBase).reduced
             : v < 0       ? dependency(v)
                           : std::set<std::size_t>{};
    };
    std::set<FunctionId> active;
    std::function<Result(FunctionId, const std::vector<std::int64_t> &)> invoke;
    invoke = [&](FunctionId id, const std::vector<std::int64_t> &args) -> Result {
      if (++work > 300000 || active.size() >= 128)
        reject(entry.declaration, "GPU matrix graph analysis limit exceeded");
      if (!active.insert(id).second)
        return {};
      const auto &fn = cpu.functions.at(id);
      std::unordered_map<SymbolId, std::int64_t> symbols;
      std::unordered_map<ValueId, std::int64_t> values;
      for (std::size_t i = 0; i < args.size(); ++i)
        symbols.emplace(fn.parameters.at(i), args[i]);
      std::function<Result(ValueId)> emit = [&](ValueId value) -> Result {
        if (++work > 300000)
          reject(entry.declaration, "GPU matrix graph analysis limit exceeded");
        if (auto found = values.find(value); found != values.end())
          return found->second;
        const auto &node = cpu.expressions.at(value);
        Result result;
        if (const auto *read = std::get_if<SymbolRead>(&node.operation)) {
          if (auto found = symbols.find(read->symbol); found != symbols.end())
            result = found->second;
        } else if (const auto *check = std::get_if<ShapeCheck>(&node.operation))
          result = emit(check->input);
        else if (const auto *matrix = std::get_if<Matmul>(&node.operation)) {
          auto left = emit(matrix->left), right = emit(matrix->right);
          if (left && right && *left >= 0 && *right >= 0 &&
              matrix->order == ReductionOrder::BalancedAdjacent &&
              matrix->separatelyRoundedProducts) {
            if (graph.inputs + graph.products.size() >= UINT32_MAX)
              reject(node.span, "GPU matrix graph representation limit exceeded");
            result = graph.inputs + graph.products.size();
            graph.products.emplace_back(*left, *right);
            graph.maps.push_back(false);
            ++graph.matrixCount;
          }
        } else if (const auto *sum = std::get_if<ReduceSum>(&node.operation)) {
          auto input = emit(sum->input);
          if (input && *input >= 0 && sum->order == ReductionOrder::BalancedAdjacent &&
              sum->rowMajor) {
            graph.reduce = true;
            dependencies.push_back({graph.reductions.size()});
            graph.reductions.push_back(*input);
            result = -1 - std::int64_t(dependencies.size());
          }
        } else if (const auto *binary = std::get_if<TensorBinary>(&node.operation)) {
          auto a = emit(binary->left), b = emit(binary->right);
          if (a && b &&
              (binary->operation == TokenKind::Plus || binary->operation == TokenKind::Minus ||
               binary->operation == TokenKind::Star || binary->operation == TokenKind::Slash)) {
            auto left = *a >= 0 ? *a : *b, right = *b >= 0 ? *b : *a;
            if (left < 0 || right < 0)
              return {};
            auto reduced = laneDependency(*a), other = laneDependency(*b);
            reduced.insert(other.begin(), other.end());
            work += reduced.size();
            if (work > 300000)
              reject(entry.declaration, "GPU reduction dependency analysis limit exceeded");
            if (laneDepth) {
              auto inputs = mergedSources(left, right);
              result = laneBase + lanes.size();
              lanes.push_back({std::move(inputs), std::move(reduced)});
              values.emplace(value, *result);
              return result;
            }
            for (auto reduction : reduced)
              graph.broadcasts.emplace_back(graph.products.size(), reduction);
            result = graph.inputs + graph.products.size();
            graph.products.emplace_back(std::min(left, right), std::max(left, right));
            graph.maps.push_back(true);
          }
        } else if (const auto *unary = std::get_if<TensorUnary>(&node.operation)) {
          auto input = emit(unary->operand);
          if (input && *input >= 0 &&
              (unary->operation == TokenKind::Plus || unary->operation == TokenKind::Minus ||
               unary->operation == TokenKind::Relu)) {
            if (laneDepth)
              result = *input;
            else {
              result = graph.inputs + graph.products.size();
              graph.products.emplace_back(*input, *input);
              graph.maps.push_back(true);
            }
          }
        } else if (std::holds_alternative<Float32Literal>(node.operation) ||
                   std::holds_alternative<BoolLiteral>(node.operation))
          result = -1;
        else if (const auto *compare = std::get_if<ScalarCompare>(&node.operation)) {
          auto a = emit(compare->left), b = emit(compare->right);
          if (a && b && *a < 0 && *b < 0) {
            auto joined = dependency(*a), right = dependency(*b);
            joined.insert(right.begin(), right.end());
            work += joined.size();
            if (work > 300000)
              reject(entry.declaration, "GPU comparison dependency analysis limit exceeded");
            if (joined.empty())
              result = -1;
            else {
              dependencies.push_back(std::move(joined));
              result = -1 - std::int64_t(dependencies.size());
            }
          }
        } else if (const auto *branch = std::get_if<ConditionalOperation>(&node.operation)) {
          const bool tensor = node.type.tensor;
          if (!(tensor ? pureLane(branch->trueResult) && pureLane(branch->falseResult)
                       : pureScalar(branch->trueResult) && pureScalar(branch->falseResult)))
            return {};
          auto condition = emit(branch->condition);
          if (tensor)
            ++laneDepth;
          auto saved = symbols;
          auto arm = [&](const std::vector<TypedStatement> &body, ValueId value) -> Result {
            for (const auto &statement : body) {
              if (!(tensor ? pureLane(statement.value) : pureScalar(statement.value)))
                return {};
              auto value = emit(statement.value);
              if (!value)
                return {};
              if (statement.binding)
                symbols[*statement.binding] = *value;
            }
            return emit(value);
          };
          auto a = arm(branch->whenTrue, branch->trueResult);
          symbols = saved;
          auto b = arm(branch->whenFalse, branch->falseResult);
          symbols = std::move(saved);
          if (tensor) {
            --laneDepth;
            if (!condition || *condition >= 0 || !a || !b || *a < 0 || *b < 0)
              return {};
            auto inputs = mergedSources(*a, *b);
            auto reduced = dependency(*condition), left = laneDependency(*a),
                 right = laneDependency(*b);
            reduced.insert(left.begin(), left.end());
            reduced.insert(right.begin(), right.end());
            work += reduced.size();
            if (work > 300000)
              reject(entry.declaration, "GPU tensor choice analysis limit exceeded");
            if (laneDepth) {
              result = laneBase + lanes.size();
              lanes.push_back({std::move(inputs), std::move(reduced)});
            } else {
              for (auto reduction : reduced)
                graph.broadcasts.emplace_back(graph.products.size(), reduction);
              result = graph.inputs + graph.products.size();
              if (inputs.size() > 2)
                graph.mapInputs.emplace(graph.products.size(),
                                        std::vector<std::uint32_t>(inputs.begin(), inputs.end()));
              graph.products.emplace_back(*inputs.begin(), *inputs.rbegin());
              graph.maps.push_back(true);
            }
          } else if (condition && a && b && *condition < 0 && *a < 0 && *b < 0) {
            auto joined = dependency(*condition);
            for (auto v : {*a, *b}) {
              auto next = dependency(v);
              joined.insert(next.begin(), next.end());
            }
            work += joined.size();
            if (work > 300000)
              reject(entry.declaration, "GPU branch dependency analysis limit exceeded");
            if (joined.empty())
              result = -1;
            else {
              dependencies.push_back(std::move(joined));
              result = -1 - std::int64_t(dependencies.size());
              graph.scalarPost = true;
            }
          }
        } else if (const auto *binary = std::get_if<ScalarBinary>(&node.operation)) {
          auto a = emit(binary->left), b = emit(binary->right);
          if (a && b && *a < 0 && *b < 0 &&
              (binary->operation == TokenKind::Plus || binary->operation == TokenKind::Minus ||
               binary->operation == TokenKind::Star || binary->operation == TokenKind::Slash)) {
            auto joined = dependency(*a), right = dependency(*b);
            joined.insert(right.begin(), right.end());
            work += joined.size();
            if (work > 300000)
              reject(entry.declaration, "GPU reduction dependency analysis limit exceeded");
            if (joined.empty())
              result = -1;
            else {
              dependencies.push_back(std::move(joined));
              result = -1 - std::int64_t(dependencies.size());
              graph.scalarPost = true;
            }
          }
        } else if (const auto *unary = std::get_if<ScalarUnary>(&node.operation)) {
          auto a = emit(unary->operand);
          if (a && *a < 0 &&
              (unary->operation == TokenKind::Plus || unary->operation == TokenKind::Minus ||
               unary->operation == TokenKind::Relu)) {
            result = *a;
            graph.scalarPost |= *result <= -2;
          }
        } else if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
          std::vector<std::int64_t> arguments;
          for (auto argument : call->arguments) {
            auto a = emit(argument);
            if (!a)
              return {};
            arguments.push_back(*a);
          }
          result = invoke(call->function, arguments);
        }
        if (result)
          values.emplace(value, *result);
        return result;
      };
      for (const auto &statement : fn.body) {
        auto value = emit(statement.value);
        if (!value)
          return {};
        if (statement.binding)
          symbols[*statement.binding] = *value;
      }
      auto result = emit(fn.result);
      active.erase(id);
      return result;
    };
    std::vector<std::int64_t> args;
    for (auto parameter : entry.parameters) {
      const auto &type = cpu.symbols[parameter].type;
      if (type.tensor && (type.shape.empty() || type.shape.size() > 8))
        return cached;
      args.push_back(type.tensor ? std::int64_t(graph.inputs++) : -1);
    }
    auto result = invoke(root, args);
    graph.tensorResult = entry.resultType.tensor && graph.reduce;
    const auto last = std::int64_t(graph.inputs + graph.products.size() - 1);
    const bool multi = graph.reductions.size() >= 2;
    auto consumed = result ? dependency(*result) : std::set<std::size_t>{};
    for (auto [stage, reduction] : graph.broadcasts)
      consumed.insert(reduction);
    if (!result || (graph.matrixCount < 2 && !multi && !graph.tensorResult) ||
        (entry.resultType.tensor
             ? (*result != last ||
                (graph.tensorResult &&
                 (graph.broadcasts.empty() || consumed.size() != graph.reductions.size())))
             : (!graph.reduce || *result > -2 || consumed.size() != graph.reductions.size() ||
                (!multi && graph.reductions.front() != last))))
      return cached;
    // Every product must contribute to the returned matrix. Extra unused
    // numerical primitives retain the existing diagnostic instead of
    // disappearing.
    std::set<std::uint32_t> live;
    if (multi || graph.tensorResult)
      live.insert(graph.reductions.begin(), graph.reductions.end());
    if (!multi || graph.tensorResult)
      live.insert(std::uint32_t(last));
    for (std::size_t i = graph.products.size(); i-- > 0;) {
      if (!live.contains(graph.inputs + i))
        return cached;
      live.insert(graph.products[i].first);
      live.insert(graph.products[i].second);
      if (auto operands = graph.mapInputs.find(i); operands != graph.mapInputs.end())
        live.insert(operands->second.begin(), operands->second.end());
    }
    const auto mapCount = std::count(graph.maps.begin(), graph.maps.end(), true);
    if (mapCount && work > 300000 / std::size_t(mapCount))
      reject(entry.declaration, "GPU graph shader expansion limit exceeded");
    cached = std::move(graph);
    return cached;
  }
  std::map<std::string, FunctionId> hostOperations;
  ExtentId hostExtent = 0;
  void observeExtents(const ValueType &type) {
    for (const auto &extent : type.shape)
      hostExtent = std::max(hostExtent, extent.identity);
  }
  static bool pointerCpuABI(const TypedModule &module, FunctionId id) {
    const auto &fn = module.functions.at(id);
    return fn.resultType.array || fn.resultType.record ||
           fn.resultType.builtin == BuiltinValue::Function ||
           std::ranges::any_of(fn.parameters, [&](auto parameter) {
             const auto &type = module.symbols.at(parameter).type;
             return type.array || type.record || type.builtin == BuiltinValue::Function;
           });
  }
  void declareCpuFunction(FunctionId id, const TypedModule *source = nullptr,
                          bool aggregates = false) {
    const auto &module = source ? *source : cpu;
    const auto &fn = module.functions.at(id);
    const bool recordABI =
        fn.resultType.record || std::ranges::any_of(fn.parameters, [&](auto parameter) {
          return module.symbols.at(parameter).type.record.has_value();
        });
    if (recordABI) {
      if (!aggregates || !declaredAggregateFunctions.emplace(source != nullptr, id).second)
        return;
      if (fn.resultType.record) {
        validateRecordSchema(module, *fn.resultType.record);
        recordType(*fn.resultType.record);
      }
      for (auto parameter : fn.parameters)
        if (auto record = module.symbols.at(parameter).type.record) {
          validateRecordSchema(module, *record);
          recordType(*record);
        }
    }
    declarations +=
        "extern \"C\" " +
        (fn.resultType.tensor || fn.resultType.record ||
                 fn.resultType.builtin == BuiltinValue::Function
             ? std::string("void")
         : fn.resultType.builtin == BuiltinValue::Bool ? std::string("bool")
                                                       : scalarType(fn.resultType.element)) +
        (source                      ? " kalandra_gpu_step_"
         : pointerCpuABI(module, id) ? " kalandra_gpu_cpu_"
                                     : " _mlir_ciface_kalandra_cpu_") +
        std::to_string(id) + "(";
    if (fn.resultType.builtin == BuiltinValue::Function)
      declarations += callableName(callableType(fn.resultType)) + "*,";
    else if (fn.resultType.record)
      declarations += recordName(*fn.resultType.record) + "*,";
    else if (fn.resultType.tensor)
      declarations +=
          tensorDescriptor(isBoolArray(fn.resultType) ? NumericType::UInt8 : fn.resultType.element,
                           fn.resultType.shape.size()) +
          "*,";
    for (auto parameter : fn.parameters) {
      const auto &type = module.symbols.at(parameter).type;
      declarations +=
          type.builtin == BuiltinValue::Function ? callableName(callableType(type)) + "*,"
          : type.record                          ? recordName(*type.record) + "*,"
          : type.tensor ? tensorDescriptor(isBoolArray(type) ? NumericType::UInt8 : type.element,
                                           type.shape.size()) +
                              "*,"
          : type.builtin == BuiltinValue::Bool ? "bool,"
                                               : scalarType(type.element) + ",";
    }
    declarations += "Invocation*);\n";
  }
  void lowerCpuModules(std::map<std::string, std::string> &outputs, SourceSpan span) {
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> linked;
    std::set<std::string> sharedNominalKeys;
    auto append = [&](const TypedModule &source, std::vector<std::string> exports,
                      const std::string &prefix, bool stepsModule) {
      auto lowered = lowerNative(source, {}, exports, arithmetic_);
      if (!lowered)
        reject(span, lowered.error());
      outputs[stepsModule ? "cpu-steps.mlir" : "cpu.mlir"] = lowered->mlir;
      llvm::SMDiagnostic diagnostic;
      auto module = llvm::parseAssemblyString(lowered->llvm, diagnostic, context);
      if (!module)
        reject(span, "native CPU bridge LLVM could not be parsed");
      std::set<llvm::Function *> bridges;
      for (std::size_t i = 0; i < exports.size(); ++i) {
        if (exports[i].empty() || (!stepsModule && !pointerCpuABI(source, i)))
          continue;
        auto *target = module->getFunction("_mlir_ciface_kalandra_cpu_" + std::to_string(i));
        if (!target)
          reject(span, "native CPU bridge entry is missing");
        auto *bridge = emitCpuPointerBridge(*target, prefix + std::to_string(i));
        bridges.insert(bridge);
      }
      // Separate typed modules have their own private callable and function IDs.
      // Only the pointer-based bridges are shared with generated C++.
      if (!stepsModule)
        for (auto &function : *module)
          if (function.getName().starts_with("kalandra_callable_"))
            function.setName(prefix + function.getName().str());
      if (stepsModule) {
        const auto targetCode = [&](FunctionId original) {
          auto imported = continuationImports.find(original);
          if (imported == continuationImports.end())
            reject(span, "GPU closure target is missing from the host continuation projection");
          auto *code = module->getFunction("kalandra_callable_" + std::to_string(imported->second));
          if (!code || code->isDeclaration())
            reject(span, "GPU closure target has no native host identity");
          code->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
          return code;
        };
        for (const auto &request : closureBridges) {
          std::vector<CpuClosureTarget> targets;
          for (auto target : request.targets)
            targets.push_back({targetCode(target), target + 1,
                               placement.functions.at(target).captureCount != 0});
          bridges.insert(emitCpuClosureBridge(*module, request.name, targets));
        }
        for (auto target : closureCodeGetters) {
          auto *type = llvm::FunctionType::get(llvm::PointerType::getUnqual(context), false);
          auto *getter = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
              "kalandra_gpu_closure_code_" + std::to_string(target), *module);
          llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", getter));
          builder.CreateRet(targetCode(target));
          bridges.insert(getter);
        }
      }
      if (stepsModule) {
        for (auto &function : *module)
          if (!function.isDeclaration() && !bridges.contains(&function))
            function.setLinkage(llvm::GlobalValue::InternalLinkage);
        for (auto &global : module->globals())
          if (!global.isDeclaration() && !global.hasAppendingLinkage())
            global.setLinkage(llvm::GlobalValue::PrivateLinkage);
      }
      // Both projections preserve the source nominal IDs. A boxed record made
      // in an ordinary helper and consumed by a continuation kernel must carry
      // the same address-significant type token. Closure environment IDs are
      // private to each projection and deliberately excluded from this join.
      for (auto &global : module->globals()) {
        auto suffix = global.getName();
        std::size_t id = 0;
        if (suffix.consume_front("kalandra_recursive_type_") && !suffix.getAsInteger(10, id) &&
            id >= source.records.size())
          global.setName(prefix + "environment_type_" + std::to_string(id));
      }
      for (std::size_t id = 0; id < source.records.size(); ++id) {
        const auto name = "kalandra_recursive_type_" + std::to_string(id);
        if (auto *token = module->getNamedGlobal(name)) {
          validateRecordSchema(source, id);
          if (!token->isConstant() || !token->hasInitializer() ||
              !token->getValueType()->isIntegerTy(8) ||
              token->getUnnamedAddr() != llvm::GlobalValue::UnnamedAddr::None)
            reject(span, "native aggregate bridge has an invalid nominal token");
          sharedNominalKeys.insert(name);
          token->setLinkage(llvm::GlobalValue::ExternalLinkage);
          if (linked && linked->getNamedGlobal(name))
            token->setInitializer(nullptr);
        }
      }
      if (llvm::verifyModule(*module))
        reject(span, "native CPU bridge ABI failed verification");
      if (!linked)
        linked = std::move(module);
      else if (llvm::Linker::linkModules(*linked, std::move(module)))
        reject(span, "native CPU bridge modules could not be linked");
    };
    if (testCpuExports_ || !usedCpuFunctions.empty()) {
      auto exports = cpuExports;
      for (std::size_t i = 0; i < exports.size(); ++i)
        if (!testCpuExports_ && !usedCpuFunctions.contains(i))
          exports[i].clear();
      append(cpu, std::move(exports), "kalandra_gpu_cpu_", false);
    }
    if (!usedContinuationCpu.empty()) {
      std::vector<std::string> exports(continuationCpu->functions.size());
      for (auto id : usedContinuationCpu)
        exports[id] = continuationCpu->functions[id].name;
      append(*continuationCpu, std::move(exports), "kalandra_gpu_step_", true);
    }
    if (linked)
      for (const auto &name : sharedNominalKeys)
        linked->getNamedGlobal(name)->setLinkage(llvm::GlobalValue::InternalLinkage);
    if (!aggregateTypes.empty() || !callableTypes.empty()) {
      if (!linked) {
        linked = std::make_unique<llvm::Module>("gpu-host-layouts", context);
        linked->setDataLayout(nativeCpuDataLayout);
        linked->setTargetTriple(llvm::Triple(KALANDRA_NATIVE_CPU_TARGET_TRIPLE));
      }
      TypedModule schema;
      schema.source = placement.source;
      schema.records = placement.records;
      std::vector<ValueType> types;
      for (const auto &[id, abi] : aggregateTypes) {
        (void)abi;
        ValueType type;
        type.builtin = BuiltinValue::Record;
        type.record = id;
        types.push_back(type);
      }
      const auto recordCount = types.size();
      types.insert(types.end(), callableTypes.begin(), callableTypes.end());
      const auto layouts =
          continuationCpu
              ? emitTaskKernelPayloadValueLayouts(schema, *continuationCpu, *linked, types)
              : emitTaskPayloadValueLayouts(schema, *linked, types);
      for (std::size_t n = 0; n < types.size(); ++n) {
        auto *layout = layouts.at(n);
        auto fields = std::vector<llvm::Constant *>();
        for (const auto &operand : layout->getInitializer()->operands())
          fields.push_back(llvm::cast<llvm::Constant>(operand.get()));
        fields[0] = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                                           n < recordCount
                                               ? UINT64_C(0x4b414c4147470000) + *types[n].record
                                               : UINT64_C(0x4b414c434c530000) + n - recordCount);
        layout->setInitializer(llvm::ConstantStruct::get(
            llvm::cast<llvm::StructType>(layout->getValueType()), fields));
        layout->setName(n < recordCount ? recordLayoutName(*types[n].record)
                                        : callableLayoutName(n - recordCount));
        layout->setLinkage(llvm::GlobalValue::ExternalLinkage);
      }
    }
    if (linked)
      for (auto id : graphNominalKeys) {
        const auto name = "kalandra_recursive_type_" + std::to_string(id);
        if (auto *token = linked->getNamedGlobal(name))
          token->setLinkage(llvm::GlobalValue::ExternalLinkage);
        else {
          auto *byte = llvm::Type::getInt8Ty(linked->getContext());
          new llvm::GlobalVariable(*linked, byte, true, llvm::GlobalValue::ExternalLinkage,
                                   llvm::ConstantInt::get(byte, 0), name);
        }
      }
    if (linked) {
      if (llvm::verifyModule(*linked))
        reject(span, "linked CPU bridge LLVM failed verification");
      llvm::raw_string_ostream text(outputs["cpu.ll"]);
      linked->print(text, nullptr);
    }
  }
  // Check a tiny operation wrapper with the common frontend, then import its
  // typed nodes with fresh IDs and the original expression's source location.
  // Only compiler-owned operator/type spellings enter this source; user text is
  // not spliced.
  FunctionId hostKernel(const std::string &operation, const std::vector<Value> &inputs,
                        SourceSpan span) {
    std::string key = operation;
    for (const auto &input : inputs) {
      completed(input, span);
      if ((input.kind != Value::Tensor && input.kind != Value::Scalar &&
           input.kind != Value::Boolean) ||
          input.gpu)
        reject(span, "inline CPU expressions require completed host values; "
                     "explicitly transfer "
                     "GPU tensors to devices.cpu() and observe tasks with try await");
      key += ":" + (input.kind == Value::Boolean
                        ? std::string("Bool")
                        : std::to_string(input.kind == Value::Tensor ? input.rank : 0) + ":" +
                              elementName(input.element));
    }
    if (auto found = hostOperations.find(key); found != hostOperations.end())
      return found->second;
    const auto conversion = resolveNumeric(operation);
    const bool builtin =
        operation == "matmul" || operation == "reduceSum" || operation == "relu" || conversion;
    if (inputs.size() != (operation == "reduceSum" || operation == "relu" || operation == "neg" ||
                                  operation == "pos" || conversion
                              ? 1
                              : 2))
      reject(span, "inline CPU operation argument count mismatch");
    auto name = "__host_expression_" + std::to_string(cpu.functions.size());
    while (cpuFunctions.contains(name))
      name += "_";
    const auto type = [](unsigned rank, NumericType element, bool tensor = false) {
      auto name = std::string(describeNumeric(element)->name);
      if (!rank && !tensor)
        return name;
      std::string result = "Tensor[" + name;
      for (unsigned axis = 0; axis < rank; ++axis)
        result += ",?";
      return result + "]";
    };
    unsigned resultRank = 0;
    bool resultTensor = false;
    std::string parameters, arguments;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      auto rank = inputs[i].kind == Value::Tensor ? inputs[i].rank : 0;
      resultRank = std::max(resultRank, rank);
      resultTensor |= inputs[i].kind == Value::Tensor;
      if (i) {
        parameters += ",";
        arguments += ",";
      }
      parameters += "a" + std::to_string(i) + ":" +
                    (inputs[i].kind == Value::Boolean
                         ? "Bool"
                         : type(rank, inputs[i].element, inputs[i].kind == Value::Tensor));
      arguments += "a" + std::to_string(i);
    }
    if (operation == "reduceSum") {
      resultRank = 0;
      resultTensor = false;
    }
    std::string body = builtin              ? operation + "(" + arguments + ")"
                       : operation == "neg" ? "-a0"
                       : operation == "pos" ? "+a0"
                                            : "a0 " + operation + " a1";
    const bool comparison = operation == "==" || operation == "!=" || operation == "<" ||
                            operation == "<=" || operation == ">" || operation == ">=";
    auto source = std::make_shared<SourceFile>(
        syntax.source->path(),
        "fn " + name + "(" + parameters + ") -> " +
            (comparison
                 ? "Bool"
                 : type(resultRank, conversion.value_or(inputs.at(0).element), resultTensor)) +
            ":\n  " + body + "\n");
    auto checked = kalandra::check(parse(source));
    if (!checked.ok()) {
      auto diagnostic = checked.diagnostics.front();
      diagnostic.span = span;
      throw Rejected{std::move(diagnostic)};
    }
    auto module = std::move(*checked.module);
    if (module.expressions.size() > SemanticLimits{}.maxValues - cpu.expressions.size())
      reject(span, "inline CPU expression value limit exceeded");
    auto fn = std::move(module.functions.at(0));
    const auto function = cpu.functions.size(), symbols = cpu.symbols.size(),
               values = cpu.expressions.size();
    std::map<ExtentId, ExtentId> extents;
    auto remapExtent = [&](Extent &extent) {
      if (!extent.identity)
        return;
      auto [entry, inserted] = extents.emplace(extent.identity, 0);
      if (inserted)
        entry->second = ++hostExtent;
      extent.identity = entry->second;
    };
    auto remapType = [&](ValueType &value) {
      for (auto &extent : value.shape)
        remapExtent(extent);
    };
    for (auto symbol : module.symbols) {
      remapType(symbol.type);
      symbol.owner = function;
      symbol.declaration = span;
      cpu.symbols.push_back(std::move(symbol));
    }
    for (auto expression : module.expressions) {
      remapType(expression.type);
      expression.span = span;
      std::visit(
          [&](auto &op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, SymbolRead>)
              op.symbol += symbols;
            else if constexpr (std::is_same_v<T, ScalarUnary> || std::is_same_v<T, TensorUnary>)
              op.operand += values;
            else if constexpr (std::is_same_v<T, ScalarBinary> ||
                               std::is_same_v<T, ScalarCompare> ||
                               std::is_same_v<T, TensorBinary> || std::is_same_v<T, Matmul>) {
              op.left += values;
              op.right += values;
            } else if constexpr (std::is_same_v<T, TensorTransform>) {
              op.input += values;
              if (op.epsilon)
                *op.epsilon += values;
            } else if constexpr (std::is_same_v<T, ReduceSum> || std::is_same_v<T, NumericConvert>)
              op.input += values;
            else
              reject(span, "unexpected operation in inline CPU kernel");
          },
          expression.operation);
      cpu.expressions.push_back(std::move(expression));
    }
    fn.declaration = span;
    remapType(fn.resultType);
    if (fn.declaredResult)
      remapType(*fn.declaredResult);
    for (auto &parameter : fn.parameters)
      parameter += symbols;
    for (auto &value : fn.expressions)
      value += values;
    fn.result += values;
    for (auto &statement : fn.body) {
      statement.span = span;
      statement.value += values;
      if (statement.binding)
        *statement.binding += symbols;
    }
    for (auto &requirement : fn.preconditions) {
      remapExtent(requirement.left);
      remapExtent(requirement.right);
    }
    cpu.functions.push_back(std::move(fn));
    cpuExports.push_back(name);
    hostOperations.emplace(key, function);
    declareCpuFunction(function);
    return function;
  }
  Value hostOperation(const std::string &operation, const std::vector<Value> &inputs,
                      SourceSpan span) {
    return cpuCall(hostKernel(operation, inputs, span), inputs, span, true);
  }
  Value cpuCall(FunctionId function, const std::vector<Value> &inputs, SourceSpan span,
                bool inlineOperation = false, bool spawned = false,
                const TypedModule *source = nullptr) {
    (source ? usedContinuationCpu : usedCpuFunctions).insert(function);
    const auto &module = source ? *source : cpu;
    const auto &fn = module.functions[function];
    if (fn.resultType.fallible)
      reject(span, "fallible CPU bridge requires the typed result adapter");
    if (!source && (containsFunctionStorage(module, fn.resultType) ||
                    std::ranges::any_of(fn.parameters, [&](auto parameter) {
                      return containsFunctionStorage(module, module.symbols.at(parameter).type);
                    })))
      reject(span, "host callable boundaries require the shared continuation projection");
    if (inputs.size() != fn.parameters.size())
      reject(span, "CPU function argument count mismatch");
    const auto supported = [](const ValueType &type) {
      return type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool ||
             type.builtin == BuiltinValue::Record ||
             (type.builtin == BuiltinValue::Function && !type.array && !type.tensor);
    };
    if (!supported(fn.resultType) || std::ranges::any_of(fn.parameters, [&](auto parameter) {
          return !supported(module.symbols.at(parameter).type);
        }))
      reject(span, "native host function requires typed transport for this value category");
    if (fn.resultType.record || std::ranges::any_of(fn.parameters, [&](auto parameter) {
          return module.symbols.at(parameter).type.record.has_value();
        }))
      declareCpuFunction(function, source, true);
    const bool callableResult = fn.resultType.builtin == BuiltinValue::Function;
    const bool recordResult = fn.resultType.record.has_value();
    const bool tensorResult = fn.resultType.tensor;
    const bool booleanResult = !tensorResult && fn.resultType.builtin == BuiltinValue::Bool;
    const bool arrayResult = fn.resultType.array;
    auto out = spawned ? std::string("value")
                       : field(callableResult ? callableName(callableType(fn.resultType))
                               : recordResult ? recordType(*fn.resultType.record)
                               : arrayResult  ? tensorDescriptor(isBoolArray(fn.resultType)
                                                                     ? NumericType::UInt8
                                                                     : fn.resultType.element,
                                                                1)
                               : tensorResult  ? "m::Buffer"
                               : booleanResult ? "bool"
                                               : scalarType(fn.resultType.element));
    Value value{callableResult  ? Value::Callable
                : recordResult  ? Value::Aggregate
                : arrayResult   ? Value::Array
                : tensorResult  ? Value::Tensor
                : booleanResult ? Value::Boolean
                                : Value::Scalar,
                spawned ? 0 : next - 1};
    value.record = fn.resultType.record;
    if (callableResult)
      value.callable = callableType(fn.resultType);
    value.array = arrayResult;
    value.booleanArray = isBoolArray(fn.resultType);
    value.element =
        (value.booleanArray || booleanResult) ? NumericType::UInt8 : fn.resultType.element;
    value.booleanTask = booleanResult;
    if (!spawned)
      trackArray(value);
    if (tensorResult) {
      value.rank = fn.resultType.shape.size();
      for (unsigned axis = 0; axis < value.rank; ++axis)
        setExtent(value, axis, fn.resultType.shape[axis].constant);
    }
    std::string step, arguments;
    std::map<ExtentId, std::pair<Value, unsigned>> extentArguments;
    std::vector<std::tuple<Value, std::uint64_t, unsigned>> fixedShapes;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      auto input = inputs[i];
      completed(input, span);
      const auto &type = module.symbols[fn.parameters[i]].type;
      if (type.builtin == BuiltinValue::Function) {
        if (input.kind != Value::Callable || input.callable != callableType(type) || input.gpu)
          reject(span, "CPU callable argument requires its declared function type");
        arguments += "&" + ref(input) + ",";
      } else if (type.record) {
        if (input.kind != Value::Aggregate || input.record != type.record || input.gpu)
          reject(span, "CPU aggregate argument requires its declared nominal type");
        arguments += "&" + ref(input) + ",";
      } else if (type.array) {
        if (input.kind != Value::Array || input.gpu || input.booleanArray != isBoolArray(type) ||
            input.element != (isBoolArray(type) ? NumericType::UInt8 : type.element))
          reject(span, "CPU array argument element type mismatch");
        arguments += "&" + ref(input) + ",";
      } else if (type.tensor) {
        if (input.kind != Value::Tensor || input.gpu)
          reject(span, "CPU tensor arguments require an explicit transfer to "
                       "devices.cpu()");
        if (type.shape.size() != input.rank || type.element != input.element)
          reject(span, "CPU tensor argument rank or element type mismatch");
        for (unsigned axis = 0; axis < input.rank; ++axis) {
          if (type.shape[axis].constant)
            fixedShapes.emplace_back(input, *type.shape[axis].constant, axis);
          else
            extentArguments.emplace(type.shape[axis].identity, std::pair{input, axis});
        }
        auto local = "argument" + std::to_string(i);
        step += "auto " + local + " = r.toCpuTyped<" + elementName(input.element) + "," +
                std::to_string(input.rank) + ">(task.context(), " + ref(input) + ");\n";
        step += "if (!" + local + ") { " + error(local + ".error()") + " }\n";
        arguments += "&*" + local + ",";
      } else {
        if (input.kind != (type.builtin == BuiltinValue::Bool ? Value::Boolean : Value::Scalar))
          reject(span, "CPU argument requires its declared numeric or Bool type");
        if (type.builtin != BuiltinValue::Bool && type.element != input.element)
          reject(span, "CPU scalar argument numeric type mismatch");
        arguments += ref(input) + ",";
      }
    }
    if (inlineOperation && tensorResult)
      for (unsigned axis = 0; axis < value.rank; ++axis) {
        const auto &dimension = fn.resultType.shape[axis];
        auto known = dimension.constant;
        if (!known) {
          auto argument = extentArguments.find(dimension.identity);
          if (argument != extentArguments.end())
            known = extent(argument->second.first, argument->second.second);
        }
        setExtent(value, axis, known);
      }
    std::string guards;
    for (const auto &[input, wanted, axis] : fixedShapes)
      guards += lengthGuard(input, wanted, span, axis);
    for (const auto &requirement : fn.preconditions) {
      const auto known = [&](Extent extent) {
        if (extent.constant)
          return extent.constant;
        const auto &argument = extentArguments.at(extent.identity);
        return Compiler::extent(argument.first, argument.second);
      };
      const auto render = [&](Extent extent) {
        return extent.constant
                   ? std::to_string(*extent.constant)
                   : ref(extentArguments.at(extent.identity).first) + ".shape()[" +
                         std::to_string(extentArguments.at(extent.identity).second) + "]";
      };
      auto left = known(requirement.left), right = known(requirement.right);
      if (left && right && left != right && !conditionalHost)
        reject(span, "CPU call violates a known tensor shape requirement");
      if (!left || !right || left != right) {
        auto [line, column] = syntax.source->location(span.begin);
        guards += "if (" + render(requirement.left) + "!=" + render(requirement.right) +
                  ") { task.fault(1," + std::to_string(line) + "," + std::to_string(column) +
                  "); return; }\n";
      }
    }
    step = guards + step;
    if (callableResult)
      step += callableName(*value.callable) + " result{};\n";
    else if (recordResult)
      step += recordName(*value.record) + " result{};\n";
    else if (tensorResult)
      step += tensorDescriptor(value.element, value.rank) + " result{};\n";
    step += (tensorResult || recordResult || callableResult ? "" : "f." + out + " = ") +
            std::string(source                            ? "kalandra_gpu_step_"
                        : pointerCpuABI(module, function) ? "kalandra_gpu_cpu_"
                                                          : "_mlir_ciface_kalandra_cpu_") +
            std::to_string(function) + "(" +
            (tensorResult || recordResult || callableResult ? "&result," : "") + arguments +
            "&task.context());\n";
    auto [line, column] = syntax.source->location(span.begin);
    const auto location = inlineOperation ? std::to_string(line) + "," + std::to_string(column)
                                          : "task.context().line(),task.context().column()";
    step += "if (task.context().status()) { task.fault(task.context().status(), " + location +
            "); return; }\n";
    if (spawned)
      step += "if(task.cancelled()) return;\n";
    if (arrayResult || recordResult || callableResult) {
      step += "f." + out + "=result;\n";
    } else if (tensorResult) {
      // Acquire the immutable backing lease before releasing the returned
      // memref owner. The buffer-deallocation pipeline gives C-interface tensor
      // results owned storage.
      step += "auto imported = r.fromCpu(task.context(), result.view());\n";
      step += "task.context().release(result.allocated);\n";
      step += "if (!imported) { " + error("imported.error()") + " }\n";
      step += "f." + out + " = *imported;\n";
    }
    for (std::size_t i = 0; i < fn.parameters.size(); ++i)
      if (module.symbols[fn.parameters[i]].type.tensor &&
          !module.symbols[fn.parameters[i]].type.array)
        step += "task.context().release(argument" + std::to_string(i) + "->allocated);\n";
    if (!spawned) {
      steps.push_back(step + commitArray(value));
      return value;
    }
    const auto prefix = "cpuJob" + std::to_string(cpuEntryCount++);
    std::string members = "m::Runtime *runtime;", captures = "f.runtime", owned = "data.runtime";
    std::string validation = "!data.runtime";
    std::set<std::size_t> captured;
    std::vector<Value> capturedArrays;
    for (const auto &input : inputs) {
      if (!captured.insert(input.slot).second)
        continue;
      const auto name = "v" + std::to_string(input.slot);
      members += valueStorage(input) + " " + name + ";";
      if (input.kind == Value::Array || input.kind == Value::Aggregate ||
          input.kind == Value::Callable)
        capturedArrays.push_back(input);
      captures += "," + ref(input);
      owned += ",data." + name;
      if (input.kind == Value::Tensor)
        validation += " || !data." + name + " || data." + name + ".onDevice() || data." + name +
                      ".format()!=*m::storageFormat(" + elementName(input.element) + ") || data." +
                      name + ".shape().size()!=" + std::to_string(input.rank);
    }
    const bool scopedJob = arrayResult || recordResult || callableResult || !capturedArrays.empty();
    std::string layouts;
    for (const auto &input : capturedArrays)
      layouts += "&" + arrayLayout(input) + ",";
    if (arrayResult || recordResult || callableResult)
      layouts += "&" + arrayLayout(value) + ",";
    if (scopedJob)
      cpuEntries += "inline const std::array<const ValueLayout*," +
                    std::to_string(capturedArrays.size() +
                                   (arrayResult || recordResult || callableResult ? 1 : 0)) +
                    "> " + prefix + "Layouts{{" + layouts + "}};\n";
    cpuEntries += "struct " + prefix + "Packet{" + members + "};\nstruct " + prefix + "Frame{" +
                  members + valueStorage(value) + " value{};" +
                  (scopedJob ? "HostArrayScopes arrayScopes{};" : "") + "};\n";
    cpuEntries += "bool " + prefix +
                  "Construct(Invocation &context,void *out,const Packet &packet){\n"
                  "const auto &data=*static_cast<const " +
                  prefix +
                  "Packet*>(packet.data);\n"
                  "if(" +
                  validation + (capturedArrays.empty() ? "" : " || !packet.source") +
                  "){context.fault(3);return false;}\n"
                  "[[maybe_unused]] auto &f=*new(out) " +
                  prefix + "Frame{" + owned + ",{}};\n";
    if (scopedJob) {
      cpuEntries += "if(!f.arrayScopes.initialize(context," + prefix + "Layouts)){f.~" + prefix +
                    "Frame();return false;}\n";
      for (std::size_t n = 0; n < capturedArrays.size(); ++n) {
        const auto &input = capturedArrays[n];
        cpuEntries += "if(!f.arrayScopes.capture(context," + std::to_string(n) + "," +
                      arrayLayout(input) + ",&" + ref(input) + ",*packet.source,&data.v" +
                      std::to_string(input.slot) + ")){f.~" + prefix + "Frame();return false;}\n";
      }
    }
    cpuEntries += "return true;}\nvoid " + prefix + "Destroy(void *frame) noexcept {static_cast<" +
                  prefix + "Frame*>(frame)->~" + prefix + "Frame();}\n";
    cpuEntries +=
        "void " + prefix + "Resume(Task &task){auto &f=*static_cast<" + prefix +
        "Frame*>(task.frame());auto &r=*f.runtime;(void)r;\nif(task.cancelled()) return;\n";
    if (scopedJob)
      cpuEntries += "HostArrayWork work(f.arrayScopes);if(!work){hostArrayFailure(task);return;}\n";
    cpuEntries += step;
    if (arrayResult || recordResult || callableResult)
      cpuEntries += "if(!f.arrayScopes.commit(" + std::to_string(capturedArrays.size()) +
                    ",&f.value)){hostArrayFailure(task);return;}\n";
    cpuEntries += "task.complete(&f.value); }\n";
    cpuEntries += "const TaskEntry " + prefix + "Entry{1," +
                  std::to_string(0x4b414c4350550000ULL + cpuEntryCount) + "ull,sizeof(" + prefix +
                  "Packet),alignof(" + prefix + "Packet),sizeof(" + prefix + "Frame),alignof(" +
                  prefix + "Frame)," + prefix + "Construct," + prefix + "Destroy," + prefix +
                  "Resume," +
                  (arrayResult || recordResult || callableResult ? "&" + arrayLayout(value)
                   : tensorResult                                ? "&m::Runtime::bufferLayout()"
                   : booleanResult                               ? "&boolScalarLayout()"
                                   : "numericScalarLayout(" + elementName(value.element) + ")") +
                  "};\n";
    auto taskField = field("TaskHandle");
    steps.push_back(prefix + "Packet packet{" + captures + "};\nf." + taskField + "=task.spawn(" +
                    activeGroup + "," + prefix + "Entry,{" + prefix +
                    "Entry.packetLayout,&packet,sizeof(packet),&task.context()});\n" +
                    admitted("f." + taskField));
    value.kind = Value::Task;
    value.scalarTask = !tensorResult && !recordResult;
    value.slot = next - 1;
    return value;
  }
  std::vector<bool> resumable;
  static std::string helperName(FunctionId id) { return "helper" + std::to_string(id); }
  Value spawnHelper(FunctionId id, const std::vector<Value> &inputs, Value target,
                    SourceSpan span) {
    if (target.kind != Value::Target || target.gpu)
      reject(span, "resumable helpers require CPU placement; GPU placements "
                   "require numeric kernels");
    const auto &fn = placement.functions.at(id);
    if (inputs.size() != fn.parameters.size())
      reject(span, "resumable helper argument count mismatch");
    std::string captures = "f.runtime,f.directory.c_str()";
    for (const auto &input : inputs) {
      completed(input, span);
      if ((input.kind != Value::Tensor && input.kind != Value::Array &&
           input.kind != Value::Scalar && input.kind != Value::Boolean &&
           input.kind != Value::Aggregate) ||
          input.gpu)
        reject(span, "resumable helper captures require completed host values");
      captures += "," + ref(input);
    }
    const auto out = field("TaskHandle");
    const auto name = helperName(id);
    steps.push_back(name + "::PacketData packet{" + captures + "};\nf." + out + "=task.spawn(" +
                    activeGroup + "," + name + "::entry,{" + name +
                    "::entry.packetLayout,&packet,sizeof(packet),&task.context()});\n" +
                    admitted("f." + out));
    Value value{Value::Task, next - 1};
    const auto type = fn.declaredResult.value_or(fn.resultType);
    if (fn.resultType.errorRecord)
      value.outcomeHelper = id;
    value.record = type.record;
    value.array = type.array;
    value.booleanArray = isBoolArray(type);
    value.booleanTask = !type.tensor && type.builtin == BuiltinValue::Bool;
    value.element = (value.booleanArray || value.booleanTask) ? NumericType::UInt8 : type.element;
    value.scalarTask = !type.tensor && !type.record;
    if (type.tensor) {
      value.rank = type.shape.size();
      for (unsigned axis = 0; axis < value.rank; ++axis)
        setExtent(value, axis, type.shape[axis].constant);
    }
    return value;
  }
  Value transfer(Value input, Value target, SourceSpan span) {
    completed(input, span);
    if (input.kind != Value::Tensor || target.kind != Value::Target)
      reject(span, "transfer requires a completed tensor and device target");
    if (target.gpu && input.gpu && input.device != target.device)
      reject(span, "cross-device transfers are not implemented");
    auto out = field("TaskHandle");
    Value result{Value::Task, next - 1, target.gpu, target.device, input.length};
    result.rank = input.rank;
    result.width = input.width;
    result.higherExtents = input.higherExtents;
    std::string step;
    if (target.gpu)
      step = "auto target = " + device(target) + ";\nif (!target) { " + error("target.error()") +
             " }\nf." + out + " = r.upload(task, " + activeGroup + ", " + ref(input) +
             ", *target);\n";
    else
      step = "f." + out + " = r.download(task, " + activeGroup + ", " + ref(input) + ");\n";
    steps.push_back(step + admitted("f." + out));
    return result;
  }
  std::vector<Value> aggregateEntryInputs(const DeviceAggregateProjection &entry,
                                           const std::vector<Value> &inputs, SourceSpan span) {
    if (inputs.size() != entry.roots.size())
      reject(span, "GPU aggregate entry argument count mismatch");
    const auto activity = field("std::array<bool," + std::to_string(entry.nodes.size()) + ">");
    std::vector<std::uint64_t> offsets(entry.nodes.size());
    std::vector<Value> values(entry.nodes.size());
    for (auto root : entry.roots) {
      const auto &node = entry.nodes[root];
      const auto &value = inputs.at(node.root);
      completed(value, span);
      if (node.type.record) {
        if (value.kind != Value::Aggregate || value.record != node.type.record || value.gpu)
          reject(span, "GPU aggregate capture requires its completed native nominal value");
        validateRecordSchema(cpu, *node.type.record);
        recordType(*node.type.record);
      }
      steps.push_back("f." + activity + "[" + std::to_string(root) + "]=true;\n");
    }
    for (std::size_t i = 0; i < entry.nodes.size(); ++i) {
      const auto &node = entry.nodes[i];
      if (node.parent == DeviceAggregateOccurrence::none) {
        if (!node.type.record)
          values[i] = inputs.at(node.root);
        continue;
      }
      const auto &parent = entry.nodes.at(node.parent);
      const auto &abi = nativeAggregateLayouts->at(*parent.type.record);
      const auto &member = abi.fields.at(node.field);
      if (member.storage == RecordFieldStorage::ManagedReference)
        reject(span, "GPU aggregate references require owned device graph transport");
      if (member.nativeOffset > UINT64_MAX - offsets[node.parent])
        reject(span, "GPU aggregate native offset exceeds the ABI");
      offsets[i] = offsets[node.parent] + member.nativeOffset;
      const auto active = "f." + activity + "[" + std::to_string(i) + "]";
      std::string code = active + "=f." + activity + "[" + std::to_string(node.parent) + "]";
      if (node.activeTag)
        code += "&&" + ref(values.at(parent.children.at(0))) + "==" +
                std::to_string(node.activeTag);
      code += ";\n";
      if (node.type.record) {
        steps.push_back(std::move(code));
        continue;
      }
      const bool boolean = node.type.builtin == BuiltinValue::Bool;
      const auto element = boolean ? NumericType::UInt8 : node.type.element;
      Value value{node.type.array ? Value::Array : boolean ? Value::Boolean : Value::Scalar};
      value.element = element;
      value.array = node.type.array;
      value.booleanArray = boolean && node.type.array;
      value.rank = node.type.shape.size();
      for (unsigned axis = 0; axis < value.rank; ++axis)
        setExtent(value, axis, node.type.shape[axis].constant);
      field(node.type.array ? tensorDescriptor(element, 1)
            : boolean       ? "bool"
                            : scalarType(element));
      value.slot = next - 1;
      values[i] = value;
      trackArray(value);
      const auto base = ref(inputs.at(node.root)) + ".data.data()";
      code += "static_assert(sizeof(" + ref(value) + ")==" +
              std::to_string(member.nativeBytes) + ");\n" + ref(value) +
              (node.discriminator ? "=1;\n" : "={};\n") + "if(" + active + "){std::memcpy(&" +
              ref(value) + "," + base + "+" + std::to_string(offsets[i]) + ",sizeof(" +
              ref(value) + "));";
      if (node.discriminator)
        code += "if(" + ref(value) + "<1||" + ref(value) + ">" +
                std::to_string(cpu.records.at(*parent.type.record).variants.size()) +
                "){task.fault(3);return;}";
      code += "}\n" + commitArray(value);
      steps.push_back(std::move(code));
    }
    std::vector<Value> result;
    for (auto leaf : entry.leaves)
      result.push_back(values.at(leaf));
    return result;
  }
  std::set<std::size_t> graphCapturePolicies, graphNominalKeys;
  std::map<std::size_t, std::vector<DeviceGraphExportLayout>> graphExportLayouts;
  std::set<FunctionId> joinedGraphPolicies, closureGraphPolicies;
  std::string graphCapturePolicy(const ValueType &type, SourceSpan span,
                                 std::span<const ValueType> joined = {}, FunctionId function = {},
                                 const ClosureGraphSchema *closure = nullptr,
                                 const TypedModule *source = nullptr) {
    const auto id = closure ? function : *type.record;
    const auto name = closure ? "GpuClosureCapture" + std::to_string(function) : joined.empty() ? "GpuGraphCapture" + std::to_string(id)
                                     : "GpuJoinedGraphCapture" + std::to_string(function);
    if (closure && !closureGraphPolicies.insert(function).second)
      return name;
    if (!closure && (joined.empty() ? !graphCapturePolicies.insert(id).second
                       : !joinedGraphPolicies.insert(function).second))
      return name;
    const auto captureType = [&](const ValueType &value) {
      if (deviceCallableValue(value))
        return callableName(callableType(value));
      if (deviceInlineRecordValue(value))
        return recordType(*value.record);
      if (deviceFlatArrayValue(value))
        return tensorDescriptor(value.builtin == BuiltinValue::Bool ? NumericType::UInt8 : value.element, 1);
      reject(span, "GPU closure capture requires an owned native graph value");
    };
    const auto record = captureType(type);
    const auto roots = joined.empty() ? std::span(&type, 1) : joined;
    auto schema = closure ? std::expected<AggregateGraphSchema, std::string>(closure->graph)
                          : planAggregateGraphSchema(source ? *source : cpu, roots, *nativeAggregateLayouts,
                                                     aggregateEntryWorkRemaining);
    if (!schema)
      reject(span, schema.error());
    if (!joined.empty() && !schema->captureJoin && !appendAggregateCaptureJoin(*schema, aggregateEntryWorkRemaining))
      reject(span, "GPU joined graph schema exceeds configured work budget");
    for (const auto &layout : schema->layouts) {
      if (!consumeAggregateProjectionWork(aggregateEntryWorkRemaining, layout.cells.size()))
        reject(span, "GPU graph capture schema exceeds configured work budget");
      for (const auto &cell : layout.cells)
        if (!consumeAggregateProjectionWork(aggregateEntryWorkRemaining, cell.guards.size()))
          reject(span, "GPU graph capture schema exceeds configured work budget");
    }
    std::string code;
    for (const auto &layout : schema->layouts)
      if (deviceInlineRecordValue(layout.type)) {
        graphNominalKeys.insert(*layout.type.record);
        code += "extern \"C\" const unsigned char kalandra_recursive_type_" +
                std::to_string(*layout.type.record) + ";\n";
      }
    if (closure) {
      for (std::size_t l = 0; l < closure->kinds.size(); ++l) {
        if (closure->kinds[l] == ClosureGraphSchema::Kind::Callable) {
          const auto bridge = "kalandra_gpu_closure_tag_" + std::to_string(closureBridges.size());
          closureBridges.push_back({bridge, closure->targets[l]});
          code += "extern \"C\" std::uint64_t " + bridge + "(const void*,const void**);\n";
          code += "static std::expected<std::uint64_t,DeviceGraphValidation> " + name +
              "_project" + std::to_string(l) + "(std::span<const std::byte> bytes){"
              "if(bytes.size()!=" + std::to_string(schema->layouts[l].nativeStride) +
              ")return std::unexpected(DeviceGraphValidation::Invalid);const void *environment=nullptr;"
              "auto tag=" + bridge + "(bytes.data(),&environment);if(!tag)return std::unexpected("
              "DeviceGraphValidation::Invalid);return tag;}\n";
        }
        if (closure->environments[l]) {
          auto target = *closure->environments[l];
          closureCodeGetters.insert(target);
          code += "extern \"C\" const void *kalandra_gpu_closure_code_" + std::to_string(target) + "();\n";
        }
      }
    }
    if (joined.empty()) {
      code += "struct " + name + " {using Record=" + record +
              ";static const ValueLayout &layout(){return " +
              (deviceCallableValue(type) ? callableLayoutName(callableType(type)) : recordLayoutName(*type.record)) + ";}\n";
    } else {
      std::string memberTypes, memberPolicies, views, policies;
      for (std::size_t i = 0; i < joined.size(); ++i) {
        if (i) {
          memberTypes += ",";
          memberPolicies += ",";
        }
        const auto member = captureType(joined[i]);
        memberTypes += member;
        if (closure) {
          const auto policy = name + "Member" + std::to_string(i);
          const auto layout = deviceCallableValue(joined[i])
              ? callableLayoutName(callableType(joined[i]))
              : deviceInlineRecordValue(joined[i]) ? recordLayoutName(*joined[i].record)
              : "hostArrayLayout<" + elementName(joined[i].builtin == BuiltinValue::Bool
                                                    ? NumericType::UInt8 : joined[i].element) + "," +
                    (joined[i].builtin == BuiltinValue::Bool ? "true" : "false") + ">()";
          policies += "struct " + policy + "{using Record=" + member +
                      ";static const ValueLayout &layout(){return " + layout + ";}};\n";
          memberPolicies += policy;
          const auto field = "std::as_bytes(std::span(&std::get<" + std::to_string(i) + ">(value),1))";
          if (joined[i].array) {
            views += "auto view" + std::to_string(i) + "=resolver(" + std::to_string(schema->roots[i]) +
                     "u," + field + ");if(!view" + std::to_string(i) +
                     ")return std::unexpected(view" + std::to_string(i) + ".error());roots[" +
                     std::to_string(i) + "]=*view" + std::to_string(i) + ";";
          } else {
            views += "roots[" + std::to_string(i) + "]={" + std::to_string(schema->roots[i]) +
                     "u,1," + field + "};";
          }
        } else {
          memberPolicies += graphCapturePolicy(joined[i], span);
          views += "{" + std::to_string(schema->roots[i]) +
                   "u,1,std::as_bytes(std::span(&std::get<" + std::to_string(i) +
                   ">(value),1))},";
        }
      }
      const auto count = std::to_string(joined.size());
      code += policies + "struct " + name + " {using Record=std::tuple<" + memberTypes + ">;\n"
              "static const ValueLayout &layout(){return deviceGraphCaptureTupleLayout<" +
              std::to_string(0x4b414c474a000000ULL + function) + "ull," + memberPolicies + ">();}\n";
      if (closure)
        code += "static std::expected<std::array<DeviceGraphNativeView," + count +
                ">,DeviceGraphValidation> captureViews(const Record &value,DeviceGraphCapture &resolver){"
                "std::array<DeviceGraphNativeView," + count + "> roots{};" + views + "return roots;}\n";
      else
        code += "static auto captureViews(const Record &value){return std::array<DeviceGraphNativeView," +
                count + ">{{" + views + "}};}\n";
    }
    const auto number = [](std::uint64_t value) { return std::to_string(value) + "ull"; };
    for (std::size_t l = 0; l < schema->layouts.size(); ++l) {
      const auto &layout = schema->layouts[l];
      const auto suffix = std::to_string(l);
      std::string wire, native;
      for (std::size_t c = 0; c < layout.cells.size(); ++c) {
        const auto &cell = layout.cells[c];
        const auto guard = "guards" + suffix + "_" + std::to_string(c);
        code += "inline static constexpr std::array<DeviceGraphGuard," +
                std::to_string(cell.guards.size()) + "> " + guard + "{{";
        for (const auto &g : cell.guards)
          code += "{" + number(g.word) + "," + number(g.tag) + "},";
        code += "}};\n";
        const auto rule = "DeviceGraphCell{" + number(cell.rule.maximum) + "," +
                          number(cell.rule.target) + "," + (cell.rule.nullable ? "true" : "false") +
                          "," + number(cell.rule.minimum) + "," + guard + "}";
        wire += rule + ",";
        native += "{" + rule + "," + number(cell.nativeOffset) + "," +
                  number(cell.nativeBytes) +
                  (closure && closure->kinds[l] == ClosureGraphSchema::Kind::Callable && c == 0
                     ? "," + name + "_project" + suffix : "") + "},";
      }
      const auto count = std::to_string(layout.cells.size());
      code += "inline static constexpr std::array<DeviceGraphCell," + count + "> cells" + suffix +
              "{{" + wire + "}};\ninline static constexpr std::array<DeviceGraphNativeCell," +
              count + "> native" + suffix + "{{" + native + "}};\n";
    }
    std::string wire, native, capture, arena, keys;
    for (std::size_t l = 0; l < schema->layouts.size(); ++l) {
      const auto &layout = schema->layouts[l];
      const auto suffix = std::to_string(l);
      const std::string singleton = layout.type.tensor ? "false" : "true";
      wire += "{cells" + suffix + "," + singleton + "},";
      native += "{native" + suffix + "," + number(layout.nativeStride) + "," + singleton + "},";
      capture += "{" + number(layout.nativeStride) + "," + number(layout.nativeAlignment) + ",";
      if (deviceInlineRecordValue(layout.type)) {
        const auto key = "&kalandra_recursive_type_" + std::to_string(*layout.type.record);
        capture += key;
        keys += key + ",";
      } else {
        capture += "nullptr";
        keys += "&cells" + suffix + ",";
      }
      if (closure && closure->environments[l])
        capture += ",kalandra_gpu_closure_code_" + std::to_string(*closure->environments[l]) + "()";
      else if (closure && closure->kinds[l] == ClosureGraphSchema::Kind::Callable)
        capture += ",nullptr,true";
      if (closure && deviceTensorValue(layout.type))
        capture += ",nullptr,false," + std::to_string(layout.type.shape.size()) + "," +
                   std::to_string(describeNumeric(layout.type.element)->bytes());
      capture += "},";
      const auto layoutId = closure ? closure->arenaLayouts[l] : schema->captureJoin == l + 1 ? 0 : layout.type.record ?
          runtime::DeviceHeapBlockABI::nominalLayout(static_cast<unsigned>(*layout.type.record), layout.type.array) :
          runtime::DeviceArrayWireABI::layout(layout.type.element, layout.type.builtin == BuiltinValue::Bool);
      arena += std::to_string(layoutId) + "u,";
      DeviceGraphExportLayout exportLayout{static_cast<unsigned>(layoutId), static_cast<unsigned>(l + 1),
                                           static_cast<unsigned>(layout.cells.size()), {}};
      for (std::size_t word = 0; word < layout.cells.size(); ++word)
        if (layout.cells[word].rule.target)
          exportLayout.references.push_back(static_cast<unsigned>(word));
      if (joined.empty() && !closure)
        graphExportLayouts[id].push_back(std::move(exportLayout));
    }
    const auto count = std::to_string(schema->layouts.size());
    code += "inline static constexpr std::array<DeviceGraphLayout," + count + "> wireLayouts{{" +
            wire + "}};\ninline static constexpr std::array<DeviceGraphNativeLayout," + count +
            "> nativeLayouts{{" + native + "}};\ninline static const std::array<DeviceGraphCaptureLayout," +
            count + "> captureLayouts{{" + capture + "}};\ninline static constexpr std::array<unsigned," +
            count + "> arenaLayouts{{" + arena + "}};\ninline static constexpr std::array<const void*," +
            count + "> keys{{" + keys + "}};\nstatic constexpr bool graphResult=true;\nstatic constexpr auto root=" +
            number(schema->captureJoin.value_or(schema->roots.at(0))) + ";};\n";
    aggregateDeclarations += std::move(code);
    return name;
  }
  std::set<std::size_t> aggregateResultPolicies;
  std::string aggregateResultPolicy(const ValueType &type, SourceSpan span) {
    const auto id = *type.record;
    const auto name = "GpuAggregateResult" + std::to_string(id);
    if (!aggregateResultPolicies.insert(id).second)
      return name;
    validateRecordSchema(cpu, id);
    const auto native = recordType(id);
    std::size_t budget = 300000;
    auto projection = projectDeviceAggregates(cpu, std::span(&type, 1), budget);
    if (!projection)
      reject(span, projection.error());
    std::vector<std::uint64_t> offsets(projection->nodes.size());
    std::string fields;
    const auto rootBytes = nativeAggregateLayouts->at(id).nativeBytes;
    for (std::size_t i = 0; i < projection->nodes.size(); ++i) {
      const auto &node = projection->nodes[i];
      const bool record = deviceInlineRecordValue(node.type);
      const bool boolean = node.type.builtin == BuiltinValue::Bool;
      const auto element = boolean || record ? NumericType::UInt8 : node.type.element;
      auto parent = std::string("DeviceAggregateResultField::None");
      auto guard = parent;
      std::size_t variants = 0;
      if (node.parent != DeviceAggregateOccurrence::none) {
        const auto &p = projection->nodes.at(node.parent);
        const auto &member = nativeAggregateLayouts->at(*p.type.record).fields.at(node.field);
        if (member.storage == RecordFieldStorage::ManagedReference ||
            member.nativeOffset > rootBytes - offsets[node.parent])
          reject(span, "GPU aggregate result requires inline native storage");
        offsets[i] = offsets[node.parent] + member.nativeOffset;
        if (!record && (member.nativeBytes != (node.type.array ? 40 : describeNumeric(element)->bytes()) ||
                        member.nativeBytes > rootBytes - offsets[i]))
          reject(span, "GPU aggregate result field does not match the native ABI");
        parent = std::to_string(node.parent);
        if (node.activeTag)
          guard = std::to_string(p.children.at(0));
        if (node.discriminator)
          variants = cpu.records.at(*p.type.record).variants.size();
      }
      fields += "{DeviceAggregateResultField::" +
                std::string(record ? "Record" : node.type.array ? "Array" : "Scalar") + "," +
                parent + "," + guard + "," + std::to_string(offsets[i]) + "," +
                std::to_string(node.activeTag) + "," + std::to_string(variants) + "," +
                elementName(element) + "," + (boolean ? "true" : "false") + "},\n";
    }
    aggregateDeclarations += "struct " + name + "{using Result=" + native +
        ";static const ValueLayout &layout(){return " + recordLayoutName(id) +
        ";}inline static constexpr std::array<DeviceAggregateResultField," +
        std::to_string(projection->nodes.size()) + "> fields{{\n" + fields + "}};};\n";
    return name;
  }
  Value spawnNumericProgram(FunctionId function, const std::vector<Value> &sourceInputs, Value target,
                            SourceSpan span, std::optional<ValueId> closureSource = {},
                            bool errorOutcome = false) {
    const auto &cpu = closureSource || errorOutcome ? placement : this->cpu;
    const auto cacheKey = errorOutcome
        ? this->cpu.functions.size() + placement.expressions.size() + function
        : closureSource ? this->cpu.functions.size() + *closureSource : function;
    admitGpuTypes(function, &cpu);
    const auto &fn = cpu.functions.at(function);
    const auto residentTensor = [](const ValueType &type) {
      return type.tensor && !type.array && type.builtin == BuiltinValue::Numeric;
    };
    auto inputs = sourceInputs;
    auto found = numericPrograms.find(cacheKey);
    std::optional<std::size_t> graphParameter;
    std::vector<std::size_t> graphPeers;
    std::string graphPolicy, graphResultPolicy;
    std::span<const DeviceGraphExportLayout> resultLayouts;
    std::optional<ClosureGraphSchema> closureSchema;
    if (closureSource) {
      const auto &task = std::get<TaskOperation>(placement.expressions.at(*closureSource).operation);
      const auto &call = std::get<FunctionCall>(placement.expressions.at(task.operand).operation);
      std::vector<ValueId> roots;
      std::vector<ValueType> rootTypes;
      for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        const auto &parameter = cpu.symbols.at(fn.parameters[i]);
        const auto &type = parameter.type;
        if (deviceCallableValue(type) || deviceInlineRecordValue(type) || deviceFlatArrayValue(type)) {
          if (parameter.mutableAccess)
            reject(span, "GPU closure graph inputs are read-only");
          if (graphParameter)
            graphPeers.push_back(i);
          else
            graphParameter = i;
          if (containsFunctionStorage(placement, type))
            roots.push_back(call.arguments.at(i));
          rootTypes.push_back(type);
        } else if (!deviceScalarValue(type) && !residentTensor(type))
          reject(span, "GPU closure entry requires native graph, scalar or resident tensor inputs");
      }
      if (!graphParameter || (!deviceScalarValue(fn.resultType) && !deviceFlatArrayValue(fn.resultType) &&
                              !deviceInlineRecordValue(fn.resultType) && !deviceTensorValue(fn.resultType)) ||
          containsFunctionStorage(placement, fn.resultType))
        reject(span, "GPU closure entry requires a scalar, flat array, tensor or non-callable record result");
      auto plan = planClosureTransfer(placement, roots, aggregateEntryWorkRemaining);
      if (!plan)
        reject(span, plan.error());
      for (const auto &environment : plan->environments)
        admitGpuTypes(environment.target, &placement);
      llvm::LLVMContext context;
      llvm::DataLayout layout(nativeCpuDataLayout);
      auto abi = planNativeClosureLayouts(placement, *plan, context, layout, aggregateEntryWorkRemaining);
      if (!abi)
        reject(span, abi.error());
      if (!nativeAggregateLayouts) {
        auto native = planNativeAggregateLayouts(placement, context, layout);
        if (!native)
          reject(span, native.error());
        nativeAggregateLayouts = std::move(*native);
      }
      auto schema = planClosureGraphSchema(placement, rootTypes, *plan, *abi,
                                           *nativeAggregateLayouts, aggregateEntryWorkRemaining);
      if (!schema)
        reject(span, schema.error());
      closureSchema = std::move(*schema);
      if (rootTypes.size() > 1) {
        if (!appendAggregateCaptureJoin(closureSchema->graph, aggregateEntryWorkRemaining))
          reject(span, "GPU joined closure graph exceeds its work budget");
        closureSchema->kinds.push_back(ClosureGraphSchema::Kind::Value);
        closureSchema->environments.emplace_back();
        closureSchema->targets.emplace_back();
        closureSchema->arenaLayouts.push_back(0);
      }
      graphPolicy = graphCapturePolicy(rootTypes.front(), span,
          rootTypes.size() > 1 ? std::span<const ValueType>(rootTypes) : std::span<const ValueType>{},
          cacheKey, &*closureSchema);
    }
    if (deviceInlineRecordValue(fn.resultType) &&
        (errorOutcome || closureSchema || !planDeviceAggregateLayouts(cpu, std::span(&fn.resultType, 1)))) {
      if (!closureSchema && std::ranges::any_of(fn.parameters, [&](auto symbol) {
            const auto &type = cpu.symbols.at(symbol).type;
            return !deviceScalarValue(type) && !deviceInlineRecordValue(type) && !residentTensor(type);
          }))
        reject(span, "GPU recursive results require scalar, nominal graph or resident tensor inputs");
      graphResultPolicy = graphCapturePolicy(fn.resultType, span, {}, {}, nullptr, &cpu);
      resultLayouts = graphExportLayouts.at(*fn.resultType.record);
    }
    const auto emit = [&](const TypedModule &module, FunctionId entry) {
      auto program = emitNumericProgram(module, entry, arithmetic_, backend_ != GpuBackend::Metal,
                                        deviceInlineRecordValue(fn.resultType), graphParameter, resultLayouts, graphPeers,
                                        closureSchema ? std::span<const DeviceClosureImport>(closureSchema->imports)
                                                      : std::span<const DeviceClosureImport>{});
      if (!program)
        throw Rejected{program.error()};
      return numericPrograms.emplace(cacheKey, std::move(*program)).first;
    };
    std::vector<const ValueType *> parameterTypes;
    if (closureSchema) {
      if (found == numericPrograms.end())
        found = emit(cpu, function);
      for (auto symbol : fn.parameters)
        parameterTypes.push_back(&cpu.symbols.at(symbol).type);
    } else if (std::ranges::any_of(fn.parameters, [&](auto symbol) {
          return cpu.symbols.at(symbol).type.record.has_value();
        })) {
      auto normalized = aggregateEntries.find(function);
      if (normalized == aggregateEntries.end()) {
        auto entry = graphResultPolicy.empty()
            ? normalizeDeviceAggregateEntry(cpu, function, aggregateEntryWorkRemaining)
            : std::expected<NormalizedDeviceEntry, std::string>(
                  std::unexpected("GPU recursive result requires nominal graph inputs"));
        if (entry) {
          found = emit(entry->module, entry->function);
          normalized = aggregateEntries.emplace(function, std::move(entry->plan)).first;
        } else {
          for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
            const auto &parameter = cpu.symbols.at(fn.parameters[i]);
            const auto &type = parameter.type;
            if (parameter.mutableAccess)
              reject(span, "GPU captured aggregate inputs are read-only");
            if (deviceInlineRecordValue(type)) {
              if (graphParameter)
                graphPeers.push_back(i);
              else
                graphParameter = i;
            } else if (!deviceScalarValue(type) && !residentTensor(type))
              reject(span, entry.error());
          }
          if (!graphParameter || (graphResultPolicy.empty() && !deviceScalarValue(fn.resultType)))
            reject(span, entry.error());
          const auto &type = cpu.symbols.at(fn.parameters[*graphParameter]).type;
          std::vector<ValueType> joined;
          if (!graphPeers.empty()) {
            joined.push_back(type);
            for (auto peer : graphPeers)
              joined.push_back(cpu.symbols.at(fn.parameters[peer]).type);
          }
          const auto roots = joined.empty() ? std::span(&type, 1) : std::span<const ValueType>(joined);
          if (graphResultPolicy.empty() && planDeviceAggregateLayouts(cpu, roots))
            reject(span, entry.error());
          graphPolicy = graphCapturePolicy(type, span, joined, function, nullptr, &cpu);
          if (found == numericPrograms.end())
            found = emit(cpu, function);
        }
      }
      if (graphParameter) {
        for (auto symbol : fn.parameters)
          parameterTypes.push_back(&cpu.symbols.at(symbol).type);
      } else {
        const auto &plan = normalized->second;
        if (plan.nodes.size() > aggregateEntryWorkRemaining)
          reject(span, "GPU aggregate host extraction exceeds configured work budget");
        aggregateEntryWorkRemaining -= plan.nodes.size();
        inputs = aggregateEntryInputs(plan, sourceInputs, span);
        for (auto leaf : plan.leaves)
          parameterTypes.push_back(&plan.nodes.at(leaf).type);
      }
    } else {
      if (found == numericPrograms.end())
        found = emit(cpu, function);
      for (auto symbol : fn.parameters)
        parameterTypes.push_back(&cpu.symbols.at(symbol).type);
    }
    const auto &program = found->second;
    std::vector<Value> tensors, scalars;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto &type = *parameterTypes.at(i);
      const auto &value = inputs[i];
      completed(value, span);
      if (i == graphParameter || std::ranges::find(graphPeers, i) != graphPeers.end()) {
        if ((deviceCallableValue(type)
                 ? value.kind != Value::Callable || value.callable != callableType(type)
                 : deviceFlatArrayValue(type)
                     ? value.kind != Value::Array || value.element != (type.builtin == BuiltinValue::Bool ? NumericType::UInt8 : type.element)
                     : value.kind != Value::Aggregate || value.record != type.record) || value.gpu)
          reject(span, "GPU graph capture requires a completed native nominal value");
        if (i == graphParameter)
          tensors.push_back(value);
        continue;
      }
      const bool boolean = type.builtin == BuiltinValue::Bool && !type.tensor && !type.array;
      if ((!boolean && !isBoolArray(type) &&
           (type.builtin != BuiltinValue::Numeric || value.element != type.element)) ||
          value.kind != (type.array    ? Value::Array
                         : boolean     ? Value::Boolean
                         : type.tensor ? Value::Tensor
                                       : Value::Scalar) ||
          (type.array && (value.gpu || value.booleanArray != isBoolArray(type))))
        reject(span, "typed numerical program argument mismatch");
      if (type.tensor && !type.array) {
        if (!value.gpu || value.device != target.device || value.rank != type.shape.size())
          reject(span,
                 "typed numerical program requires explicit matching device tensor transfers");
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (type.shape[axis].constant)
            requireLength(value, *type.shape[axis].constant, span, axis);
      }
      ((type.tensor || type.array) ? tensors : scalars).push_back(value);
    }
    auto locate = [&](const Extent &extent) -> std::pair<std::size_t, unsigned> {
      std::size_t tensor = 0;
      for (std::size_t i = 0; i < parameterTypes.size(); ++i) {
        if (i == graphParameter) {
          ++tensor;
          continue;
        }
        const auto &type = *parameterTypes[i];
        if (!type.tensor)
          continue;
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (!extent.constant && type.shape[axis].identity == extent.identity)
            return {tensor, axis};
        ++tensor;
      }
      reject(span, "typed numerical program shape contract cannot be mapped to an input");
    };
    for (const auto &requirement : fn.preconditions) {
      if (requirement.left.constant && requirement.right.constant) {
        if (requirement.left.constant != requirement.right.constant)
          reject(span, "typed numerical program shape contract is inconsistent");
      } else if (requirement.left.constant || requirement.right.constant) {
        const auto &dynamic = requirement.left.constant ? requirement.right : requirement.left;
        const auto &constant = requirement.left.constant ? requirement.left : requirement.right;
        auto [input, axis] = locate(dynamic);
        requireLength(tensors.at(input), *constant.constant, span, axis);
      } else {
        auto [a, ax] = locate(requirement.left);
        auto [b, bx] = locate(requirement.right);
        auto left = extent(tensors.at(a), ax), right = extent(tensors.at(b), bx);
        if (left && right && left != right && !conditionalHost)
          reject(span, "typed numerical program input shapes differ");
        if (!left || !right || left != right) {
          auto [line, column] = syntax.source->location(span.begin);
          steps.push_back("if(" + ref(tensors[a]) + ".shape()[" + std::to_string(ax) + "]!=" +
                          ref(tensors[b]) + ".shape()[" + std::to_string(bx) + "]){task.fault(1," +
                          std::to_string(line) + "," + std::to_string(column) + ");return;}\n");
        }
      }
    }
    const auto [entry, inserted] = entries.emplace(cacheKey, entries.size());
    (void)inserted;
    const auto base = "kernel" + std::to_string(entry->second);
    auto out = field("TaskHandle");
    Value result{Value::Task, next - 1, true, target.device};
    result.array = program.type.array;
    result.record = program.type.record;
    result.booleanArray = isBoolArray(program.type);
    result.gpu = !result.array && !result.record;
    result.scalarTask = !program.type.tensor && !result.array && !result.record;
    result.booleanTask = program.type.builtin == BuiltinValue::Bool && !program.type.tensor;
    result.element = result.booleanTask || result.booleanArray ? NumericType::UInt8 : program.type.element;
    result.rank = program.type.shape.size();
    for (unsigned axis = 0; axis < result.rank; ++axis)
      setExtent(result, axis, program.type.shape[axis].constant);
    auto schema = [&](const auto &types) {
      std::string text = "std::array<kalandra::NumericType," + std::to_string(types.size()) + ">{";
      for (auto type : types)
        text += elementName(type) + ",";
      return text + "}";
    };
    auto indices = [&](const auto &values, const std::string &type) {
      std::string text = "std::array<" + type + "," + std::to_string(values.size()) + ">{";
      for (auto value : values)
        text += std::to_string(value) + ",";
      return text + "}";
    };
    auto capture = [&](const Value &value) {
      if (value.kind == Value::Boolean)
        return "std::uint64_t(" + ref(value) + " ? 1 : 0)";
      return "std::uint64_t(std::bit_cast<std::uint" +
             std::to_string(describeNumeric(value.element)->bits) + "_t>(" + ref(value) + "))";
    };
    const auto executable = field("m::NumericExecutable");
    std::string bindings, captureList;
    std::size_t captureCount = 0;
    for (std::size_t i = 0; i < program.code.size(); ++i) {
      const auto &op = program.code[i];
      const auto suffix = std::to_string(i);
      bindings += "auto scalars" + suffix + "=std::array<std::uint64_t," +
                  std::to_string(op.scalarSources.size() + op.constants.size()) + ">{";
      for (auto source : op.scalarSources)
        bindings += capture(scalars.at(source)) + ",";
      for (auto value : op.constants)
        bindings += std::to_string(value) + "ull,";
      bindings += "};\n";
      if (op.kind == NumericProgramKind::Map || isGpuControlKind(op.kind))
        for (std::size_t j = 0; j < op.scalarSources.size() + op.constants.size(); ++j) {
          captureList += "scalars" + suffix + "[" + std::to_string(j) + "],";
          ++captureCount;
        }
    }
    std::string code = bindings + "auto target=" + device(target) + ";if(!target){" +
                       error("target.error()") + "}\n" + "if(!f." + executable + ".on(*target)){\n";
    for (std::size_t i = 0; i < program.kernels.size(); ++i) {
      const auto &kernel = program.kernels[i];
      const auto name = "kernel" + std::to_string(i);
      const auto path = base + (i ? "-stage" + std::to_string(i) : "") +
                        (backend_ == GpuBackend::Vulkan ? ".spv"
                         : backend_ == GpuBackend::Cuda ? ".ptx"
                                                        : ".metallib");
      auto instruction = std::find_if(program.code.begin(), program.code.end(),
                                      [i](const auto &op) { return op.kernel == i; });
      if (instruction == program.code.end())
        reject(span, "unreferenced typed numerical program kernel");
      const auto kind = instruction->kind;
      if (kernel.transform) {
        const char *label = *kernel.transform == TensorTransformKind::Transpose     ? "Transpose"
                            : *kernel.transform == TensorTransformKind::SoftmaxRows ? "SoftmaxRows"
                                                                                    : "RmsNormRows";
        code += "auto " + name + "=r.loadTensorTransformKernel(f.directory+\"/" + path +
                "\",*target,kalandra::TensorTransformKind::" + label + "," +
                elementName(kernel.element) + "," +
                (kernel.scalarBuffers.size() > 1 && kernel.scalarBuffers[1] ? "true" : "false") +
                ");if(!" + name + "){" + error(name + ".error()") + "}\n";
        continue;
      }
      code += "auto " + name + "=r." +
              (kind == NumericProgramKind::Reduce   ? "loadNumericReductionKernel"
               : kind == NumericProgramKind::Matmul ? "loadNumericMatmulKernel"
                                                    : "loadNumericGraphKernel") +
              "(f.directory+\"/" + path + "\",*target," + elementName(kernel.element);
      if (kind == NumericProgramKind::Map || isGpuControlKind(kind))
        code += ",m::ElementwiseSignature{" + std::to_string(kernel.tensorTypes.size()) + "," +
                std::to_string(kernel.scalarTypes.size()) +
                (kernel.indirect ? ",m::ElementwiseBinding::ArgumentBuffer}," : "},") +
                schema(kernel.tensorTypes) + "," + schema(kernel.scalarTypes) + "," +
                indices(kernel.reads, "std::uint32_t") + "," +
                indices(kernel.scalarBuffers, "std::uint8_t");
      code += ");if(!" + name + "){" + error(name + ".error()") + "}\n";
    }
    constexpr std::array names{"Map",         "Reduce",     "Matmul",        "Branch",
                               "Jump",        "Alias",      "EqualShape",    "Extent",
                               "MatrixShape", "ShapeFault", "ScalarControl", "DeviceControl"};
    for (std::size_t i = 0; i < program.code.size(); ++i) {
      const auto &op = program.code[i];
      const auto suffix = std::to_string(i);
      code += "auto operands" + suffix + "=" + indices(op.inputs, "std::uint32_t") + ";\n";
    }
    code += "std::array<m::NumericInstruction," + std::to_string(program.code.size()) + "> code{{";
    for (std::size_t i = 0; i < program.code.size(); ++i) {
      const auto &op = program.code[i];
      const auto suffix = std::to_string(i);
      code +=
          "{m::NumericInstructionKind::" + std::string(names.at(static_cast<unsigned>(op.kind))) +
          "," + (op.kernel == UINT32_MAX ? "m::Kernel{}" : "*kernel" + std::to_string(op.kernel)) +
          ",operands" + suffix + ",scalars" + suffix + "," + std::to_string(op.output) + "u," +
          std::to_string(op.kind == NumericProgramKind::Map && op.kernel != UINT32_MAX &&
                                 program.kernels[op.kernel].noArithmeticFault
                             ? 1u
                             : op.target) +
          "u" + (op.workPerElement ? "," + std::to_string(op.workPerElement) + "u" : "") + "},";
    }
    code += "}};\nauto prepared=r.prepareNumericProgram(" + std::to_string(tensors.size()) +
            "u,code," + std::to_string(program.result) + "u," +
            elementName(result.array || result.record ? NumericType::UInt64 : result.element) + "," +
            (result.scalarTask ? "true" : "false") + (result.booleanTask ? ",true" : "") +
            (backend_ != GpuBackend::Metal ? ",*target" : "") + ");if(!prepared){" +
            error("prepared.error()") + "}f." + executable + "=*prepared;\n}\n";
    const bool arrayInputs =
        std::ranges::any_of(tensors, [](const auto &v) { return v.kind == Value::Array; });
    if (arrayInputs || graphParameter || result.array || result.record) {
      std::string arguments;
      for (const auto &value : tensors) {
        if (graphParameter && value.slot == inputs.at(*graphParameter).slot)
          arguments += ",DeviceGraphArgument<" + graphPolicy + ">";
        else if (value.kind == Value::Tensor)
          arguments += ",DeviceResidentArgument<m::Runtime,m::Buffer>";
        else
          arguments += ",DeviceArrayArgument<" + elementName(value.element) + "," +
                       (value.booleanArray ? "true" : "false") + ">";
      }
      code += "using ArrayJob=" + std::string(result.record ? "DeviceAggregateResultJob" :
                                             result.array ? "DeviceArrayResultJob" :
                                             !result.scalarTask ? "DeviceTensorResultJob" : "DeviceArrayInputJob") +
              "<m::Runtime,m::Buffer,m::Device,m::NumericExecutable," +
              std::to_string(0x4b414c4441520000ULL + function) + "ull,";
      if (result.record)
        code += (graphResultPolicy.empty() ? aggregateResultPolicy(program.type, span)
                                           : graphResultPolicy) + ",";
      else
        code += elementName(result.element) + "," +
                ((result.array ? result.booleanArray : result.booleanTask) ? "true" : "false") + ",";
      code +=
              std::to_string(captureCount) + arguments + ">;\n";
      code += "ArrayJob::PacketData packet{&r,f." + executable + ",*target," +
              "std::array<std::uint64_t," + std::to_string(captureCount) + ">{" + captureList +
              "},";
      code += "std::tuple{";
      for (const auto &value : tensors) {
        if (graphParameter && value.slot == inputs.at(*graphParameter).slot && !graphPeers.empty()) {
          code += "std::tuple{" + ref(value);
          for (auto peer : graphPeers)
            code += "," + ref(inputs.at(peer));
          code += "},";
        } else
          code += ref(value) + ",";
      }
      code += "}";
      code += "};\nf." + out + "=task.spawn(" + activeGroup +
              ",ArrayJob::entry,{ArrayJob::entry.packetLayout,&packet,sizeof(packet),&task.context("
              ")});\n";
    } else {
      code += "f." + out + "=r.executeNumericProgram(task," + activeGroup + ",f." + executable +
              ",std::array<m::Buffer," + std::to_string(tensors.size()) + ">{";
      for (const auto &value : tensors)
        code += ref(value) + ",";
      code += "},std::array<std::uint64_t," + std::to_string(captureCount) + ">{" + captureList +
              "});\n";
    }
    code += admitted("f." + out);
    steps.push_back(std::move(code));
    if (errorOutcome)
      result.outcomeRecord = result.record;
    return result;
  }
  Value spawnNumeric(FunctionId function, const std::vector<Value> &inputs, Value target,
                     SourceSpan span) {
    admitGpuTypes(function);
    const auto &fn = cpu.functions.at(function);
    if (backend_ != GpuBackend::Metal || numericPrograms.contains(function) ||
        fn.resultType.builtin == BuiltinValue::Bool ||
        std::ranges::any_of(fn.parameters, [&](auto id) {
          return cpu.symbols.at(id).type.builtin == BuiltinValue::Bool ||
                 cpu.symbols.at(id).type.array || cpu.symbols.at(id).type.record;
        }))
      return spawnNumericProgram(function, inputs, target, span);
    auto found = numericShaders.find(function);
    if (found == numericShaders.end()) {
      auto shader = emitNumericShader(cpu, function, {.arithmetic = arithmetic_});
      if (!shader)
        return spawnNumericProgram(function, inputs, target, span);
      found = numericShaders.emplace(function, std::move(*shader)).first;
    }
    const auto element = found->second.element;
    std::vector<Value> tensors, scalars;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto &value = inputs[i];
      const auto &type = cpu.symbols.at(fn.parameters.at(i)).type;
      completed(value, span);
      if (value.element != type.element ||
          value.kind != (type.tensor ? Value::Tensor : Value::Scalar))
        reject(span, "typed GPU argument category or numerical type mismatch");
      if (type.tensor) {
        if (!value.gpu || value.device != target.device || value.rank != type.shape.size())
          reject(span, "typed GPU tensors require matching ranks and explicit "
                       "transfer to the "
                       "selected device");
        for (unsigned axis = 0; axis < value.rank; ++axis)
          if (type.shape[axis].constant)
            requireLength(value, *type.shape[axis].constant, span, axis);
      }
      (type.tensor ? tensors : scalars).push_back(value);
    }
    auto locate = [&](const Extent &extent) -> std::pair<std::size_t, unsigned> {
      std::size_t tensor = 0;
      for (auto parameter : fn.parameters) {
        const auto &type = cpu.symbols.at(parameter).type;
        if (!type.tensor)
          continue;
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (!extent.constant && extent.identity == type.shape[axis].identity)
            return {tensor, axis};
        ++tensor;
      }
      reject(span, "typed GPU shape contract cannot be mapped to an input");
    };
    for (const auto &requirement : fn.preconditions) {
      if (requirement.left.constant && requirement.right.constant) {
        if (requirement.left.constant != requirement.right.constant)
          reject(span, "typed GPU shape contract is inconsistent");
      } else if (requirement.left.constant || requirement.right.constant) {
        const auto &dynamic = requirement.left.constant ? requirement.right : requirement.left;
        const auto &constant = requirement.left.constant ? requirement.left : requirement.right;
        auto [input, axis] = locate(dynamic);
        requireLength(tensors.at(input), *constant.constant, span, axis);
      } else {
        auto [a, ax] = locate(requirement.left);
        auto [b, bx] = locate(requirement.right);
        auto left = extent(tensors.at(a), ax), right = extent(tensors.at(b), bx);
        if (left && right && left != right && !conditionalHost)
          reject(span, "typed GPU argument shapes differ");
        if (!left || !right || left != right) {
          auto [line, column] = syntax.source->location(span.begin);
          steps.push_back("if(" + ref(tensors[a]) + ".shape()[" + std::to_string(ax) + "]!=" +
                          ref(tensors[b]) + ".shape()[" + std::to_string(bx) + "]){task.fault(1," +
                          std::to_string(line) + "," + std::to_string(column) + ");return;}\n");
        }
      }
    }
    const auto [entry, inserted] = entries.emplace(function, entries.size());
    (void)inserted;
    auto kernel = field("m::Kernel"), out = field("TaskHandle");
    const auto &reads = found->second.reads;
    auto result = tensors.empty() ? Value{Value::Task} : tensors.at(reads.front());
    result.kind = Value::Task;
    result.slot = next - 1;
    result.element = element;
    result.scalarTask = !found->second.reduction.empty() || found->second.scalar;
    std::string buffers = "std::array<m::Buffer," + std::to_string(tensors.size()) + ">{";
    for (const auto &value : tensors)
      buffers += ref(value) + ",";
    buffers += "}";
    std::string capture = "std::array<std::uint64_t," + std::to_string(scalars.size()) + ">{";
    for (const auto &value : scalars)
      capture += "std::uint64_t(std::bit_cast<std::uint" +
                 std::to_string(describeNumeric(value.element)->bits) + "_t>(" + ref(value) + ")),";
    capture += "}";
    std::string projection = "std::array<std::uint32_t," + std::to_string(reads.size()) + ">{";
    for (auto read : reads)
      projection += std::to_string(read) + ",";
    projection += "}";
    std::string reduction;
    if (!found->second.reduction.empty())
      reduction = "auto reduction=r.loadNumericReductionKernel(f.directory+\"/kernel" +
                  std::to_string(entry->second) + "-reduce.metallib\",*target," +
                  elementName(element) + ");if(!reduction){" + error("reduction.error()") + "}\n";
    if (found->second.matrix) {
      if (tensors.size() != 2 || !scalars.empty())
        reject(span, "typed matrix capture layout mismatch");
      result.rank = 2;
      result.length = tensors[0].length;
      result.width = tensors[1].width;
      steps.push_back("auto target=" + device(target) + ";if(!target){" + error("target.error()") +
                      "}\n"
                      "auto kernel=r.loadNumericMatmulKernel(f.directory+\"/kernel" +
                      std::to_string(entry->second) + ".metallib\",*target," +
                      elementName(element) + ");if(!kernel){" + error("kernel.error()") + "}\n" +
                      reduction + "f." + out +
                      (reduction.empty() ? "=r.matmul(task," : "=r.matmulReduceSum(task,") +
                      activeGroup + ",*kernel," + (reduction.empty() ? "" : "*reduction,") +
                      ref(tensors[0]) + "," + ref(tensors[1]) + ");\n" + admitted("f." + out));
      return result;
    }
    auto schema = [&](const std::vector<NumericType> &types) {
      std::string result =
          "std::array<kalandra::NumericType," + std::to_string(types.size()) + ">{";
      for (auto type : types)
        result += elementName(type) + ",";
      return result + "}";
    };
    steps.push_back(
        "auto target=" + device(target) + ";if(!target){" + error("target.error()") + "}\n" +
        "auto kernel=r.loadNumericKernel(f.directory+\"/kernel" + std::to_string(entry->second) +
        ".metallib\",*target," + elementName(element) + ",m::ElementwiseSignature{" +
        std::to_string(tensors.size()) + "," + std::to_string(scalars.size()) +
        (found->second.indirect ? ",m::ElementwiseBinding::ArgumentBuffer}," : "},") +
        schema(found->second.tensorTypes) + "," + schema(found->second.scalarTypes) + "," +
        projection + ");\n" + "if(!kernel){" + error("kernel.error()") + "}\n" + reduction + "f." +
        kernel + "=*kernel;\nf." + out + "=r.numericElementwise(task," + activeGroup + ",f." +
        kernel + "," + buffers + "," + capture +
        (found->second.scalar ? ",{},true"
         : reduction.empty()  ? ""
                              : ",*reduction") +
        ");\n" + admitted("f." + out));
    return result;
  }
  Value spawn(FunctionId function, const std::vector<Value> &inputs, Value target,
              SourceSpan span) {
    if (!target.gpu)
      return cpuCall(function, inputs, span, false, true);
    if (backend_ != GpuBackend::Metal || cpu.functions.at(function).resultType.record)
      return spawnNumericProgram(function, inputs, target, span);
    bool mixed = false;
    bool conditional = false, primitive = false, transform = false, control = false;
    std::set<FunctionId> inspected;
    std::vector<FunctionId> pending{function};
    while (!pending.empty()) {
      auto id = pending.back();
      pending.pop_back();
      if (!inspected.insert(id).second)
        continue;
      control |= usesScalarControl(cpu, id);
      for (auto value : cpu.functions.at(id).expressions) {
        const auto &node = cpu.expressions.at(value);
        conditional |= std::holds_alternative<ConditionalOperation>(node.operation);
        transform |= std::holds_alternative<TensorTransform>(node.operation);
        primitive |= std::holds_alternative<Matmul>(node.operation) ||
                     std::holds_alternative<ReduceSum>(node.operation);
        if (std::holds_alternative<NumericConvert>(node.operation) ||
            (node.type.builtin == BuiltinValue::Numeric &&
             node.type.element != NumericType::Float32))
          mixed = true;
        if (auto call = std::get_if<FunctionCall>(&node.operation))
          pending.push_back(call->function);
      }
    }
    bool selectedProgram = transform || control;
    if ((conditional || primitive) && !matrixGraph(function) && !primitiveEntry(function))
      selectedProgram |=
          primitive || !emitNumericShader(cpu, function, {.arithmetic = arithmetic_}).has_value();
    // A legacy graph may recognize the primitives while its fused map cannot
    // prove branch-local shapes. Preflight those maps before generating host
    // state so the selected-stage program can carry the actual obligations.
    if (conditional && !selectedProgram && !mixed) {
      try {
        if (const auto &graph = matrixGraph(function); graph) {
          for (std::size_t stage = 0; stage < graph->maps.size(); ++stage)
            if (graph->maps[stage])
              shader(function, nullptr, false, -1, false, stage);
          if (!graph->tensorResult && graph->reduce &&
              (graph->reductions.size() >= 2 || graph->scalarPost))
            shader(function, nullptr, true, -1, true);
        } else if (const auto entry = primitiveEntry(function); entry) {
          if (entry->mapped)
            shader(function);
          if (entry->leftMapped)
            shader(function, nullptr, false, 0);
          if (entry->rightMapped)
            shader(function, nullptr, false, 1);
          if (entry->post)
            shader(function, nullptr, true);
          if (entry->scalarPost)
            shader(function, nullptr, true, -1, true);
        } else {
          shader(function);
        }
      } catch (const Rejected &) {
        selectedProgram = true;
      }
    }
    if (selectedProgram || mixed ||
        (cpu.functions.at(function).resultType.tensor &&
         cpu.functions.at(function).resultType.shape.empty()) ||
        std::any_of(cpu.functions.at(function).parameters.begin(),
                    cpu.functions.at(function).parameters.end(),
                    [&](auto p) {
                      const auto &t = cpu.symbols.at(p).type;
                      return t.tensor && t.shape.empty();
                    }) ||
        (!cpu.functions.at(function).resultType.tensor &&
         std::none_of(cpu.functions.at(function).parameters.begin(),
                      cpu.functions.at(function).parameters.end(),
                      [&](auto p) { return cpu.symbols.at(p).type.tensor; })) ||
        cpu.functions.at(function).resultType.element != NumericType::Float32 ||
        std::any_of(cpu.functions.at(function).parameters.begin(),
                    cpu.functions.at(function).parameters.end(), [&](auto parameter) {
                      const auto &type = cpu.symbols.at(parameter).type;
                      return type.builtin == BuiltinValue::Numeric &&
                             type.element != NumericType::Float32;
                    }))
      return spawnNumeric(function, inputs, target, span);
    admitGpuTypes(function);
    const auto &entryFunction = cpu.functions[function];
    const auto &graph = matrixGraph(function);
    const bool sum = reduction(function);
    const bool matrix = matrixFunction(function);
    const bool matrixSum = sum && matrix;
    const bool mapped = sum && primitiveEntry(function)->mapped;
    const bool post = (sum || matrix) && primitiveEntry(function)->post;
    if (!entryFunction.resultType.tensor && !sum && !(graph && graph->reduce))
      reject(span, "GPU scalar entries currently require one reduceSum with "
                   "forwarding helpers");
    std::vector<Value> tensors, scalars;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto &value = inputs[i];
      const auto &type = cpu.symbols[entryFunction.parameters[i]].type;
      if (!type.tensor && type.builtin != BuiltinValue::Numeric)
        reject(span, "GPU scalar captures currently require float32; Boolean "
                     "conditions stay "
                     "internal to helpers");
      const bool tensor = type.tensor;
      completed(value, span);
      if (value.kind != (tensor ? Value::Tensor : Value::Scalar))
        reject(span, "GPU argument type mismatch");
      if (tensor && value.rank != type.shape.size())
        reject(span, "GPU tensor ranks must match their parameters");
      if (tensor)
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (type.shape[axis].constant)
            requireLength(value, *type.shape[axis].constant, span, axis);
      (tensor ? tensors : scalars).push_back(value);
    }
    if (tensors.empty())
      reject(span, "elementwise GPU entries currently require at least one "
                   "tensor input");
    if (tensors.size() > UINT32_MAX || scalars.size() >= UINT32_MAX)
      reject(span, "GPU argument representation limit exceeded");
    std::vector<std::uint32_t> reads;
    if (!graph && (!sum || mapped) && !matrix)
      shader(function, &reads);
    else
      for (std::size_t i = 0; i < tensors.size(); ++i)
        reads.push_back(i);
    for (auto value : tensors)
      if (!value.gpu || value.device != target.device)
        reject(span, "GPU arguments must be explicitly transferred to the "
                     "selected device");
    auto left = tensors[reads.front()], right = tensors.back();
    if (auto entry = primitiveEntry(function); entry && (sum || matrix)) {
      left = tensors.at(entry->left);
      if (matrix)
        right = tensors.at(entry->right);
    }
    // Parameter extent identities already include transitive helper
    // obligations.
    auto locate = [&](const Extent &wanted) -> std::pair<std::size_t, unsigned> {
      std::size_t tensor = 0;
      for (auto parameter : entryFunction.parameters) {
        const auto &type = cpu.symbols[parameter].type;
        if (!type.tensor)
          continue;
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
          if (!wanted.constant && wanted.identity == type.shape[axis].identity)
            return {tensor, axis};
        ++tensor;
      }
      reject(span, "GPU shape requirement cannot be mapped to a parameter");
    };
    for (const auto &requirement : entryFunction.preconditions) {
      if (requirement.left.constant && requirement.right.constant) {
        if (requirement.left.constant != requirement.right.constant)
          reject(span, "GPU argument shapes differ");
        continue;
      }
      if (requirement.left.constant || requirement.right.constant) {
        const auto &dynamic = requirement.left.constant ? requirement.right : requirement.left;
        const auto &constant = requirement.left.constant ? requirement.left : requirement.right;
        auto [input, axis] = locate(dynamic);
        requireLength(tensors[input], *constant.constant, span, axis);
        continue;
      }
      auto [a, axisA] = locate(requirement.left);
      auto [b, axisB] = locate(requirement.right);
      auto aExtent = extent(tensors[a], axisA), bExtent = extent(tensors[b], axisB);
      if (aExtent && bExtent && aExtent != bExtent && !conditionalHost)
        reject(span, (matrix || graph) ? "matmul inner dimensions differ"
                                       : "elementwise argument shapes differ");
      if (!aExtent || !bExtent || aExtent != bExtent) {
        auto [line, column] = syntax.source->location(span.begin);
        steps.push_back("if(" + ref(tensors[a]) + ".shape()[" + std::to_string(axisA) +
                        "]!=" + ref(tensors[b]) + ".shape()[" + std::to_string(axisB) +
                        "]) {task.fault(1," + std::to_string(line) + "," + std::to_string(column) +
                        ");return;}\n");
      }
    }
    if (graph) {
      auto [entry, inserted] = entries.emplace(function, entries.size());
      (void)inserted;
      auto kernel = field("m::Kernel"), out = field("TaskHandle");
      Value result{Value::Task, next - 1, true, target.device};
      result.rank = entryFunction.resultType.tensor ? entryFunction.resultType.shape.size() : 2;
      result.scalarTask = graph->reduce && !graph->tensorResult;
      std::vector<Value> shapes = tensors;
      for (std::size_t i = 0; i < graph->products.size(); ++i) {
        auto [a, b] = graph->products[i];
        Value product = shapes.at(a);
        if (!graph->maps[i])
          product.width = shapes.at(b).width;
        shapes.push_back(product);
      }
      result.length = shapes.back().length;
      result.width = shapes.back().width;
      result.higherExtents = shapes.back().higherExtents;
      std::string inputs = "std::array<m::Buffer," + std::to_string(tensors.size()) + ">{";
      for (std::size_t i = 0; i < tensors.size(); ++i)
        inputs += (i ? "," : "") + ref(tensors[i]);
      inputs += "}";
      std::string products =
          "std::array<m::MatrixProduct," + std::to_string(graph->products.size()) + ">{{";
      std::string operandTables;
      for (std::size_t i = 0; i < graph->products.size(); ++i) {
        auto [a, b] = graph->products[i];
        products += "{" + std::to_string(a) + "," + std::to_string(b);
        if (auto operands = graph->mapInputs.find(i); operands != graph->mapInputs.end()) {
          const auto name = "mapInputs" + std::to_string(i);
          operandTables += "static constexpr auto " + name + "=std::array<std::uint32_t," +
                           std::to_string(operands->second.size()) + ">{";
          for (auto input : operands->second)
            operandTables += std::to_string(input) + ",";
          operandTables += "};\n";
          products += "," + name;
        }
        products += "},";
      }
      products += "}}";
      std::string stages, extra;
      if (graph->reduce) {
        const auto reductionKernel = field("m::Kernel");
        stages = "auto reduction=r.loadReductionKernel(f.directory+\"/kernel" +
                 std::to_string(entry->second) + "-reduce.metallib\",*target);\nif(!reduction){" +
                 error("reduction.error()") + "}\nf." + reductionKernel + "=*reduction;\n";
        extra = ",f." + reductionKernel;
        if (!graph->tensorResult && graph->scalarPost && graph->reductions.size() < 2) {
          const auto scalarKernel = field("m::Kernel");
          stages += "auto scalar=r.loadElementwiseKernel(f.directory+\"/kernel" +
                    std::to_string(entry->second) +
                    "-scalar.metallib\",*target,m::ElementwiseSignature{1," +
                    std::to_string(scalars.size()) + "});\nif(!scalar){" + error("scalar.error()") +
                    "}\nf." + scalarKernel + "=*scalar;\n";
          extra +=
              ",f." + scalarKernel + ",std::array<float," + std::to_string(scalars.size()) + ">{";
          for (std::size_t i = 0; i < scalars.size(); ++i)
            extra += (i ? "," : "") + ref(scalars[i]);
          extra += "}";
        }
      }
      if (graph->tensorResult) {
        extra += ",std::array<std::uint32_t," + std::to_string(graph->reductions.size()) + ">{";
        for (auto root : graph->reductions)
          extra += std::to_string(root) + ",";
        extra += "}";
      } else if (graph->reductions.size() >= 2) {
        auto joinKernel = field("m::Kernel");
        std::vector<std::uint32_t> joinReads;
        shader(function, &joinReads, true);
        const bool indirect =
            scalarControl(function) ||
            !kalandra::runtime::metal::directElementwiseLayout(joinReads.size(), scalars.size());
        std::string projection;
        if (joinReads.size() != graph->reductions.size()) {
          projection = ",std::array<std::uint32_t," + std::to_string(joinReads.size()) + ">{";
          for (auto read : joinReads)
            projection += std::to_string(read) + ",";
          projection += "}";
        }
        stages += "auto join=r.loadElementwiseKernel(f.directory+\"/kernel" +
                  std::to_string(entry->second) +
                  "-join.metallib\",*target,m::ElementwiseSignature{" +
                  std::to_string(graph->reductions.size()) + "," + std::to_string(scalars.size()) +
                  (indirect ? ",m::ElementwiseBinding::ArgumentBuffer}" : "}") + projection +
                  ");\nif(!join){" + error("join.error()") + "}\nf." + joinKernel + "=*join;\n";
        extra += ",f." + joinKernel + ",std::array<std::uint32_t," +
                 std::to_string(graph->reductions.size()) + ">{";
        for (std::size_t i = 0; i < graph->reductions.size(); ++i)
          extra += (i ? "," : "") + std::to_string(graph->reductions[i]);
        extra += "},std::array<float," + std::to_string(scalars.size()) + ">{";
        for (std::size_t i = 0; i < scalars.size(); ++i)
          extra += (i ? "," : "") + ref(scalars[i]);
        extra += "}";
      }
      std::string graphMaps;
      if (std::any_of(graph->maps.begin(), graph->maps.end(), [](bool map) { return map; })) {
        graphMaps = "std::array<m::Kernel," + std::to_string(graph->products.size()) + ">{";
        for (std::size_t i = 0; i < graph->products.size(); ++i) {
          if (i)
            graphMaps += ",";
          if (!graph->maps[i]) {
            graphMaps += "m::Kernel{}";
            continue;
          }
          std::vector<std::uint32_t> reads;
          shader(function, &reads, false, -1, false, i);
          auto mapKernel = field("m::Kernel");
          stages += "auto map" + std::to_string(i) +
                    "=r.loadElementwiseKernel(f.directory+\"/kernel" +
                    std::to_string(entry->second) + "-map" + std::to_string(i) +
                    ".metallib\",*target,m::ElementwiseSignature{" + std::to_string(reads.size()) +
                    "," + std::to_string(scalars.size()) +
                    (!scalarControl(function) && kalandra::runtime::metal::directElementwiseLayout(
                                                     reads.size(), scalars.size())
                         ? "});\nif(!map"
                         : ",m::ElementwiseBinding::ArgumentBuffer});\nif(!map") +
                    std::to_string(i) + "){" + error("map" + std::to_string(i) + ".error()") +
                    "}\nf." + mapKernel + "=*map" + std::to_string(i) + ";\n";
          graphMaps += "f." + mapKernel;
        }
        graphMaps += "},std::array<float," + std::to_string(scalars.size()) + ">{";
        for (std::size_t i = 0; i < scalars.size(); ++i)
          graphMaps += (i ? "," : "") + ref(scalars[i]);
        graphMaps += "}";
        if (!graph->tensorResult && graph->reduce && !graph->scalarPost &&
            graph->reductions.size() < 2)
          extra += ",m::Kernel{},std::span<const float>{}";
        extra += "," + graphMaps;
      }
      if (!graph->broadcasts.empty()) {
        extra += ",std::array<m::MatrixScalar," + std::to_string(graph->broadcasts.size()) + ">{{";
        for (auto [stage, reduction] : graph->broadcasts)
          extra += "{" + std::to_string(stage) + "," + std::to_string(reduction) + "},";
        extra += "}}";
      }
      steps.push_back("auto target=" + device(target) + ";\nif(!target){" +
                      error("target.error()") + "}\nauto kernel=r." +
                      (graph->matrixCount ? "loadMatmulKernel" : "loadReductionKernel") +
                      "(f.directory+\"/kernel" + std::to_string(entry->second) +
                      ".metallib\",*target);\nif(!kernel){" + error("kernel.error()") + "}\nf." +
                      kernel + "=*kernel;\n" + stages + operandTables +
                      "static constexpr auto products=" + products + ";\nf." + out + "=r." +
                      (graph->tensorResult             ? "graphTensor"
                       : graph->reductions.size() >= 2 ? "graphReductions"
                       : graph->reduce                 ? "matmulGraphReduceSum"
                                                       : "matmulGraph") +
                      "(task," + activeGroup + ",f." + kernel + "," + inputs + ",products" + extra +
                      ");\n" + admitted("f." + out));
      return result;
    }
    const bool projected = reads.size() != tensors.size();
    const bool indirect =
        (!sum || mapped) && !matrix &&
        (retainedChoiceBindings(function, reads.size()) ||
         !kalandra::runtime::metal::directElementwiseLayout(reads.size(), scalars.size()));
    const bool legacy = !projected && !indirect && tensors.size() == 2 && scalars.empty();
    auto signature = ", m::ElementwiseSignature{" + std::to_string(tensors.size()) + "," +
                     std::to_string(scalars.size()) +
                     (indirect ? ",m::ElementwiseBinding::ArgumentBuffer}" : "}");
    if (projected) {
      signature += ", std::array<std::uint32_t," + std::to_string(reads.size()) + ">{";
      for (std::size_t i = 0; i < reads.size(); ++i)
        signature += (i ? "," : "") + std::to_string(reads[i]);
      signature += "}";
    }
    auto pack = [&](const std::vector<Value> &values, std::string type) {
      std::string result = "std::array<" + type + "," + std::to_string(values.size()) + ">{";
      for (std::size_t i = 0; i < values.size(); ++i)
        result += (i ? ", " : "") + ref(values[i]);
      return result + "}";
    };
    auto arguments = legacy ? ref(left) + ", " + ref(right)
                            : pack(tensors, "m::Buffer") + ", " + pack(scalars, "float");
    auto [entry, inserted] = entries.emplace(function, entries.size());
    (void)inserted;
    auto kernel = field("m::Kernel"), out = field("TaskHandle");
    Value result{Value::Task, next - 1, true, target.device, left.length};
    result.rank = left.rank;
    result.width = matrix ? right.width : left.width;
    result.higherExtents = left.higherExtents;
    std::string mappedSetup;
    if (mapped) {
      const auto reductionKernel = field("m::Kernel");
      mappedSetup = "auto reduction = r.loadReductionKernel(f.directory + \"/kernel" +
                    std::to_string(entry->second) + "-reduce.metallib\", *target);\n" +
                    "if(!reduction){" + error("reduction.error()") + "}\nf." + reductionKernel +
                    "=*reduction;\n";
      arguments = "f." + reductionKernel + ", " + pack(tensors, "m::Buffer") + ", " +
                  pack(scalars, "float");
    }
    if (matrix)
      arguments = ref(left) + ", " + ref(right);
    if (matrixSum) {
      const auto reductionKernel = field("m::Kernel");
      mappedSetup += "auto reduction = r.loadReductionKernel(f.directory + \"/kernel" +
                     std::to_string(entry->second) + "-reduce.metallib\", *target);\n" +
                     "if(!reduction){" + error("reduction.error()") + "}\nf." + reductionKernel +
                     "=*reduction;\n";
      arguments = "f." + reductionKernel + ", " + arguments;
    }
    std::string matrixAdjustmentArguments;
    if (post) {
      std::vector<std::uint32_t> postReads;
      shader(function, &postReads,
             true); // Validate before publishing any host/shader artifacts.
      const auto epilogue = field("m::Kernel");
      const auto count = matrix ? tensors.size() + 1 : 1;
      const bool indirectPost =
          retainedChoiceBindings(function, postReads.size()) ||
          !kalandra::runtime::metal::directElementwiseLayout(postReads.size(), scalars.size());
      std::string postSignature = ", m::ElementwiseSignature{" + std::to_string(count) + "," +
                                  std::to_string(scalars.size()) +
                                  (indirectPost ? ",m::ElementwiseBinding::ArgumentBuffer}" : "}");
      if (postReads.size() != count) {
        postSignature += ", std::array<std::uint32_t," + std::to_string(postReads.size()) + ">{";
        for (std::size_t i = 0; i < postReads.size(); ++i)
          postSignature += (i ? "," : "") + std::to_string(postReads[i]);
        postSignature += "}";
      }
      mappedSetup += "auto post = r.loadElementwiseKernel(f.directory + \"/kernel" +
                     std::to_string(entry->second) + "-post.metallib\", *target" + postSignature +
                     ");\nif(!post){" + error("post.error()") + "}\nf." + epilogue + "=*post;\n";
      if (!mapped && !matrix)
        arguments = ref(left);
      auto &postArguments = matrixSum ? matrixAdjustmentArguments : arguments;
      postArguments += ", f." + epilogue;
      if (matrix) {
        auto postInputs = tensors;
        postInputs.insert(postInputs.begin(),
                          left); // Replaced by the completed matrix in the task frame.
        postArguments += ", " + pack(postInputs, "m::Buffer");
      }
      if (!mapped)
        postArguments += ", " + pack(scalars, "float");
    }
    if (matrix && (primitiveEntry(function)->leftMapped || primitiveEntry(function)->rightMapped)) {
      // Both operand maps read the original immutable argument pack. Their
      // projected read tables and shapes are independent.
      if (!post && !matrixSum)
        arguments += ", m::Kernel{}, std::span<const m::Buffer>{}, "
                     "std::span<const float>{}";
      for (int operand = 0; operand < 2; ++operand) {
        const bool enabled = operand == 0 ? primitiveEntry(function)->leftMapped
                                          : primitiveEntry(function)->rightMapped;
        if (!enabled) {
          arguments += ", m::Kernel{}";
          continue;
        }
        std::vector<std::uint32_t> mapReads;
        shader(function, &mapReads, false, operand);
        const auto mapKernel = field("m::Kernel");
        const auto label = operand == 0 ? "left" : "right";
        const bool indirectMap =
            retainedChoiceBindings(function, mapReads.size()) ||
            !kalandra::runtime::metal::directElementwiseLayout(mapReads.size(), scalars.size());
        std::string mapSignature = ", m::ElementwiseSignature{" + std::to_string(tensors.size()) +
                                   "," + std::to_string(scalars.size()) +
                                   (indirectMap ? ",m::ElementwiseBinding::ArgumentBuffer}" : "}");
        mapSignature += ", std::array<std::uint32_t," + std::to_string(mapReads.size()) + ">{";
        for (std::size_t i = 0; i < mapReads.size(); ++i)
          mapSignature += (i ? "," : "") + std::to_string(mapReads[i]);
        mapSignature += "}";
        mappedSetup +=
            "auto " + std::string(label) + "Map = r.loadElementwiseKernel(f.directory + \"/kernel" +
            std::to_string(entry->second) + "-" + label + ".metallib\", *target" + mapSignature +
            ");\nif(!" + label + "Map){" + error(std::string(label) + "Map.error()") + "}\nf." +
            mapKernel + "=*" + label + "Map;\n";
        arguments += ", f." + mapKernel;
      }
      arguments += ", " + pack(tensors, "m::Buffer") + ", " + pack(scalars, "float");
    }
    if (matrixSum && post) {
      if (!primitiveEntry(function)->leftMapped && !primitiveEntry(function)->rightMapped)
        arguments += ", m::Kernel{}, m::Kernel{}, std::span<const "
                     "m::Buffer>{}, std::span<const float>{}";
      arguments += matrixAdjustmentArguments;
    }
    if (matrixSum && primitiveEntry(function)->scalarPost) {
      if (!primitiveEntry(function)->leftMapped && !primitiveEntry(function)->rightMapped && !post)
        arguments += ", m::Kernel{}, m::Kernel{}, std::span<const "
                     "m::Buffer>{}, std::span<const float>{}";
      if (!post)
        arguments += ", m::Kernel{}, std::span<const m::Buffer>{}, "
                     "std::span<const float>{}";
      shader(function, nullptr, true, -1, true);
      const auto scalarKernel = field("m::Kernel");
      mappedSetup += "auto scalarPost = r.loadElementwiseKernel(f.directory + \"/kernel" +
                     std::to_string(entry->second) +
                     "-scalar.metallib\", *target, m::ElementwiseSignature{1," +
                     std::to_string(scalars.size()) + "});\nif(!scalarPost){" +
                     error("scalarPost.error()") + "}\nf." + scalarKernel + "=*scalarPost;\n";
      arguments += ", f." + scalarKernel + ", " + pack(scalars, "float");
    }
    steps.push_back("auto target = " + device(target) + ";\nif (!target) { " +
                    error("target.error()") + " }\n" + std::string("auto kernel = r.") +
                    (matrix           ? "loadMatmulKernel"
                     : sum && !mapped ? "loadReductionKernel"
                                      : "loadElementwiseKernel") +
                    "(f.directory + \"/kernel" + std::to_string(entry->second) +
                    ".metallib\", *target" +
                    ((legacy && !mapped) || (sum && !mapped) || matrix ? "" : signature) +
                    ");\n"
                    "if (!kernel) { " +
                    error("kernel.error()") + " }\nf." + kernel + " = *kernel;\n" + mappedSetup +
                    "f." + out + " = r." +
                    (matrixSum ? "matmulReduceSum"
                     : matrix  ? "matmul"
                     : mapped  ? "elementwiseReduceSum"
                     : sum     ? "reduceSum"
                               : "elementwise") +
                    "(task, " + activeGroup + ", f." + kernel + ", " +
                    (sum && !matrix && !mapped && !post ? ref(left) : arguments) + ");\n" +
                    admitted("f." + out));
    result.scalarTask = sum;
    return result;
  }
  Value numericKernel(ValueId id, const std::vector<Value> &args) {
    const auto &typed = node(id);
    const auto span = typed.span;
    if (const auto *compare = std::get_if<ScalarCompare>(&typed.operation)) {
      const auto operation = compare->operation;
      return hostOperation(operation == ComparisonKind::Equal       ? "=="
                           : operation == ComparisonKind::NotEqual  ? "!="
                           : operation == ComparisonKind::Less      ? "<"
                           : operation == ComparisonKind::LessEqual ? "<="
                           : operation == ComparisonKind::Greater   ? ">"
                                                                    : ">=",
                           args, span);
    }
    if (const auto *literal = std::get_if<TensorLiteral>(&typed.operation)) {
      const auto rank = typed.type.shape.size();
      if (rank > 8)
        reject(span, "the Metal source bridge requires rank-zero through "
                     "rank-eight numeric tensors");
      const auto rows = rank ? typed.type.shape[0].constant.value() : 1;
      const auto &elements = literal->elements;
      for (auto item : elements)
        if (!literalBits(item))
          reject(span, "Metal tensor literals require constant numeric values");
      auto out = field("m::Buffer");
      Value value{Value::Tensor, next - 1, false, 0, rows};
      value.rank = rank;
      value.element = typed.type.element;
      const auto width = describeNumeric(value.element)->bytes();
      for (unsigned axis = 0; axis < rank; ++axis)
        setExtent(value, axis, typed.type.shape[axis].constant);
      std::string data = "{ ";
      for (auto item : elements)
        data += std::to_string(*literalBits(item)) + "ull,";
      data += " }";
      std::vector<std::uint64_t> strides(rank, 1);
      const bool empty = std::any_of(typed.type.shape.begin(), typed.type.shape.end(),
                                     [](const auto &axis) { return axis.constant == 0; });
      if (!empty)
        for (std::size_t axis = rank; axis > 1; --axis)
          strides[axis - 2] = strides[axis - 1] * typed.type.shape[axis - 1].constant.value();
      std::string dimensions, layout;
      for (unsigned axis = 0; axis < rank; ++axis) {
        dimensions += (axis ? "," : "") + std::to_string(typed.type.shape[axis].constant.value());
        layout += (axis ? "," : "") + std::to_string(strides[axis]);
      }
      auto descriptor = tensorDescriptor(value.element, rank) +
                        " descriptor{allocation,allocation,0" +
                        (rank ? ",{" + dimensions + "},{" + layout + "}" : "") + "};\n";
      steps.push_back("static const std::array<std::uint" + std::to_string(width * 8) + "_t," +
                      std::to_string(elements.size()) + "> data=" + data + ";\n" +
                      "auto *allocation=static_cast<" + scalarType(value.element) +
                      "*>(task.context().allocate(data.size()*" + std::to_string(width) +
                      "));\nif(!allocation){task.fault(2);return;}\n" +
                      "if(!data.empty()) std::memcpy(allocation,data.data(),data.size()*" +
                      std::to_string(width) + ");\n" + descriptor +
                      "auto value=r.fromCpu(task.context(),descriptor.view());\n" +
                      "task.context().release(allocation);\nif(!value){" + error("value.error()") +
                      "}\nf." + out + "=*value;\n");
      return value;
    }
    if (const auto *shape = std::get_if<ShapeCheck>(&typed.operation)) {
      auto value = args.at(0);
      for (const auto &requirement : shape->requirements) {
        const auto first = steps.size();
        requireLength(value, requirement.extent, span, requirement.axis);
        if (value.failure)
          for (auto index = first; index < steps.size(); ++index)
            steps[index] =
                "if(" + failureRef(value) + ".kind==TaskFailure::None){\n" + steps[index] + "}\n";
        setExtent(value, requirement.axis, requirement.extent);
      }
      return value;
    }
    if (const auto *unary = std::get_if<ScalarUnary>(&typed.operation))
      return hostOperation(unary->operation == TokenKind::Relu    ? "relu"
                           : unary->operation == TokenKind::Minus ? "neg"
                                                                  : "pos",
                           args, span);
    if (const auto *binary = std::get_if<ScalarBinary>(&typed.operation))
      return arithmetic(binary->operation, args, span);
    if (std::holds_alternative<NumericConvert>(typed.operation))
      return hostOperation(std::string(describeNumeric(typed.type.element)->name), args, span);
    if (const auto *transform = std::get_if<TensorTransform>(&typed.operation))
      return hostOperation(tensorTransformName(transform->kind), args, span);
    if (const auto *unary = std::get_if<TensorUnary>(&typed.operation))
      return hostOperation(unary->operation == TokenKind::Relu    ? "relu"
                           : unary->operation == TokenKind::Minus ? "neg"
                                                                  : "pos",
                           args, span);
    if (const auto *binary = std::get_if<TensorBinary>(&typed.operation))
      return arithmetic(binary->operation, args, span);
    if (std::holds_alternative<ReduceSum>(typed.operation))
      return hostOperation("reduceSum", args, span);
    if (std::holds_alternative<Matmul>(typed.operation))
      return hostOperation("matmul", args, span);
    reject(span, "unsupported numerical continuation operation");
  }
  Value arithmetic(TokenKind operation, const std::vector<Value> &args, SourceSpan span) {
    if (operation != TokenKind::Plus && operation != TokenKind::Minus &&
        operation != TokenKind::Star && operation != TokenKind::Slash &&
        operation != TokenKind::Percent)
      reject(span, "unsupported inline numeric arithmetic");
    return hostOperation(operation == TokenKind::Plus      ? "+"
                         : operation == TokenKind::Minus   ? "-"
                         : operation == TokenKind::Star    ? "*"
                         : operation == TokenKind::Percent ? "%"
                                                           : "/",
                         args, span);
  }
  std::vector<std::string> plannedGroups;
  std::vector<std::size_t> plannedPC;
  struct GpuAwaitSite {
    std::size_t instruction, group;
  };
  std::vector<GpuAwaitSite> gpuAwaitSites;
  std::vector<Value> plannedParameters;
  static Value applicationError(Value result) {
    const auto [family, slot] = *result.applicationError;
    Value value{Value::Aggregate, slot};
    value.record = family;
    return value;
  }
  std::string nativeField(Value record, std::size_t field) {
    recordType(*record.record);
    return ref(record) + ".data.data()+" +
           std::to_string(aggregateTypes.at(*record.record).fields.at(field).nativeOffset);
  }
  std::string readNativeField(Value record, std::size_t member, Value output) {
    const auto source = nativeField(record, member);
    const auto &abi = aggregateTypes.at(*record.record).fields.at(member);
    if (abi.storage == RecordFieldStorage::ManagedReference || output.kind == Value::Tensor)
      reject({}, "GPU host field requires an owned reference or tensor adapter");
    return "static_assert(sizeof(" + ref(output) + ")==" + std::to_string(abi.nativeBytes) +
           ");\nstd::memcpy(&" + ref(output) + "," + source + ",sizeof(" + ref(output) + "));\n";
  }
  std::string hostErrorTag(Value value) {
    return "([&](){std::int64_t tag=0;std::memcpy(&tag," + nativeField(value, 0) +
           ",sizeof(tag));return tag;}())";
  }
  std::string hostErrorCopy(Value output, Value input) {
    if (output.slot == input.slot)
      return {};
    if (output.record == input.record)
      return ref(output) + "=" + ref(input) + ";\n";
    const auto taskFamily = taskErrorFamily(placement);
    const auto fromId = input.record ? input.record : taskFamily;
    const auto toId = output.record ? output.record : taskFamily;
    if (!fromId || !toId)
      reject({}, "host error conversion requires checked error families");
    const auto &from = placement.records.at(*fromId);
    const auto &to = placement.records.at(*toId);
    if (!consumeAggregateProjectionWork(aggregateEntryWorkRemaining, from.variants.size()) ||
        !consumeAggregateProjectionWork(aggregateEntryWorkRemaining, to.variants.size()))
      reject({}, "GPU host error conversion exceeds configured work budget");
    std::string code = "{std::int64_t sourceTag=0;\n";
    if (input.record)
      code += "std::memcpy(&sourceTag," + nativeField(input, 0) + ",sizeof(sourceTag));\n";
    else
      code += "sourceTag=static_cast<unsigned>(" + ref(input) + ".kind);\n";
    code += ref(output) + "={};\nswitch(sourceTag){\n";
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> tags;
    for (std::size_t v = 0; v < to.variants.size(); ++v)
      tags.emplace(errorOrigin(placement, *toId, v), v);
    for (std::size_t v = 0; v < from.variants.size(); ++v) {
      auto found = tags.find(errorOrigin(placement, *fromId, v));
      if (found == tags.end())
        continue;
      const auto target = found->second;
      code += "case " + std::to_string(v + 1) + ":{\n";
      if (output.record) {
        code += "const std::int64_t tag=" + std::to_string(target + 1) + ";\nstd::memcpy(" +
                nativeField(output, 0) + ",&tag,sizeof(tag));\n";
        const auto &fields = from.variants[v].fields;
        const auto &targetFields = to.variants[target].fields;
        if (fields.size() != targetFields.size())
          reject({}, "host error conversion changed its payload fields");
        for (std::size_t n = 0; n < fields.size(); ++n) {
          if (!consumeAggregateProjectionWork(aggregateEntryWorkRemaining, 1))
            reject({}, "GPU host error conversion exceeds configured work budget");
          const auto destination = nativeField(output, targetFields[n]);
          const auto bytes = aggregateTypes.at(*output.record).fields.at(targetFields[n]).nativeBytes;
          if (input.record) {
            const auto source = nativeField(input, fields[n]);
            if (aggregateTypes.at(*input.record).fields.at(fields[n]).nativeBytes != bytes)
              reject({}, "host error conversion changed its payload layout");
            code += "std::memcpy(" + destination + "," + source + "," +
                    std::to_string(bytes) + ");\n";
          } else
            code += "static_assert(sizeof(Failure)==" + std::to_string(bytes) +
                    ");\nstd::memcpy(" + destination + ",&" + ref(input) + ",sizeof(Failure));\n";
        }
      } else {
        code += readNativeField(input, from.variants[v].fields.front(), output);
        code += "if(static_cast<unsigned>(" + ref(output) + ".kind)!=" +
                std::to_string(target + 1) + "){task.fault(3);return;}\n";
      }
      code += "break;}\n";
    }
    return code + "default:task.fault(3);return;}}\n";
  }
  Value lowerPlan(const continuation::Plan &plan, FunctionId entryId,
                  bool publishApplicationOutcome = false) {
    using namespace continuation;
    for (const auto &type : plan.slots)
      if (type.array && type.builtin != BuiltinValue::Numeric && !isBoolArray(type))
        reject({}, "mixed GPU host continuations require numeric/Bool arrays or a typed "
                   "aggregate transport");
    std::map<Slot, Value> values;
    std::map<Slot, unsigned> payloadWrites;
    std::map<std::size_t, std::size_t> joins;
    std::map<Slot, std::size_t> gpuCalls;
    for (std::size_t i = 0; i < plan.code.size(); ++i)
      if (plan.code[i].code == Code::Join)
        joins.emplace(plan.code[i].group, i);
    for (std::size_t i = 0; i < plan.groups; ++i)
      plannedGroups.push_back("f." + field("GroupHandle"));
    auto shape = [&](Value &value, const ValueType &type) {
      if (type.builtin == BuiltinValue::Numeric)
        value.element = type.element;
      value.record = type.record;
      if (type.builtin == BuiltinValue::Function)
        value.callable = callableType(type);
      value.array = type.array;
      value.booleanArray = isBoolArray(type);
      if (value.booleanArray)
        value.element = NumericType::UInt8;
      value.gpu = type.device.has_value();
      value.device = type.device.value_or(0);
      if (type.tensor) {
        value.rank = type.shape.size();
        for (unsigned axis = 0; axis < value.rank; ++axis)
          setExtent(value, axis, type.shape[axis].constant);
      }
    };
    auto allocate = [&](Slot slot) {
      const auto &type = plan.slots.at(slot);
      if (type.builtin == BuiltinValue::Function && (type.array || type.tensor))
        reject({}, "host callable arrays require an owned array transport layout");
      Value value{type.task                                ? Value::Task
                  : type.array                             ? Value::Array
                  : type.tensor                            ? Value::Tensor
                  : type.record                            ? Value::Aggregate
                  : type.builtin == BuiltinValue::Function ? Value::Callable
                                                           : Value::Scalar};
      shape(value, type);
      std::string storage = type.task     ? "TaskHandle"
                            : type.array  ? tensorDescriptor(value.element, 1)
                            : type.tensor ? "m::Buffer"
                            : type.record ? recordType(*type.record)
                            : type.builtin == BuiltinValue::Function
                                ? callableName(callableType(type))
                                : scalarType(type.element);
      if (type.builtin == BuiltinValue::Device) {
        value.kind = Value::Target;
      } else {
        if (!type.task && !type.array && type.builtin == BuiltinValue::Bool) {
          value.kind = Value::Boolean;
          storage = "bool";
        }
        if (type.builtin == BuiltinValue::TaskError) {
          value.kind = Value::Error;
          storage = "Failure";
        }
        if (type.builtin == BuiltinValue::FaultKind) {
          value.kind = Value::FaultKind;
          storage = "unsigned";
        }
        if (type.builtin == BuiltinValue::String) {
          value.kind = Value::Text;
          storage = "const char *";
        }
        field(storage);
        value.slot = next - 1;
      }
      trackArray(value);
      value.scalarTask = type.task && !type.tensor && !type.record;
      value.booleanTask = value.scalarTask && type.builtin == BuiltinValue::Bool;
      if (value.booleanTask)
        value.element = NumericType::UInt8;
      if (type.fallible) {
        field("Failure");
        value.failure = next - 1;
      }
      if (type.errorRecord && type.fallible && !type.task) {
        field(recordType(*type.errorRecord));
        value.applicationError = {*type.errorRecord, next - 1};
      }
      return value;
    };
    auto get = [&](Slot slot) -> Value {
      auto found = values.find(slot);
      if (found != values.end())
        return found->second;
      return values.emplace(slot, allocate(slot)).first->second;
    };
    auto payloadCopy = [](Value output, Value input) {
      return ref(output) + "=" + ref(input) + ";\n";
    };
    auto copy = [&](Value output, Value input) {
      auto code = payloadCopy(output, input);
      if (input.applicationError)
        code = "if(" + failureRef(input) + ".kind==TaskFailure::None && " +
               hostErrorTag(applicationError(input)) + "==0){\n" + code + "}else{" +
               ref(output) + "={};}\n";
      if (output.failure)
        code +=
            failureRef(output) + "=" + (input.failure ? failureRef(input) : "Failure{}") + ";\n";
      if (output.applicationError) {
        const auto error = applicationError(output);
        if (input.applicationError) {
          const auto original = applicationError(input);
          if (error.slot != original.slot)
            code += "if(" + hostErrorTag(original) + "!=0){\n" +
                    hostErrorCopy(error, original) + "}else{" + ref(error) + "={};}\n";
        } else
          code += ref(error) + "={};\n";
      }
      return code + commitArray(output);
    };
    // Backend kernels can prove a result shape more precisely than its public
    // type. Carry those facts through values, but meet all payload-producing
    // branch writes rather than letting the last arm dictate a merge shape.
    auto write = [&](Slot slot, Value input) {
      auto output = get(slot);
      const auto &type = plan.slots.at(slot);
      const bool first = (type.task || type.tensor) && payloadWrites[slot]++ == 0;
      if (type.task) {
        if (!first &&
            (output.outcomeHelper != input.outcomeHelper ||
             output.outcomeRecord != input.outcomeRecord))
          reject({}, "task branches require a common application outcome layout");
        output.outcomeHelper = input.outcomeHelper;
        output.outcomeRecord = input.outcomeRecord;
        values[slot] = output;
      }
      if (type.tensor) {
        for (unsigned axis = 0; axis < output.rank; ++axis) {
          auto known = type.shape[axis].constant;
          if (!known)
            known = first                                         ? extent(input, axis)
                    : extent(output, axis) == extent(input, axis) ? extent(output, axis)
                                                                  : std::nullopt;
          setExtent(output, axis, known);
        }
        values[slot] = output;
      }
      return output;
    };
    auto bind = [&](Slot slot, Value input) {
      auto found = values.find(slot);
      if (found != values.end()) {
        const auto output = write(slot, input);
        steps.push_back(copy(output, input));
      } else {
        const auto original = input;
        shape(input, plan.slots.at(slot));
        if (input.kind == Value::Tensor || (input.kind == Value::Task && !input.scalarTask)) {
          for (unsigned axis = 0; axis < input.rank; ++axis)
            if (!extent(input, axis))
              setExtent(input, axis, extent(original, axis));
        }
        values.emplace(slot, input);
        payloadWrites[slot] = 1;
      }
    };
    // Parameter storage belongs to the child frame. Declared fixed dimensions
    // are checked here; inferred operation guards stay inside their source
    // scopes.
    const auto &fn = placement.functions.at(entryId);
    std::string guards;
    auto [line, column] = syntax.source->location(fn.declaration.begin);
    const auto fault =
        "{task.fault(1," + std::to_string(line) + "," + std::to_string(column) + ");return;}\n";
    for (std::size_t n = 0; n < plan.parameters.size(); ++n) {
      const auto parameter = get(plan.parameters[n]);
      plannedParameters.push_back(parameter);
      const auto &type = placement.symbols.at(fn.parameters[n]).type;
      for (std::size_t axis = 0; axis < type.shape.size(); ++axis) {
        const auto actual = ref(parameter) + ".shape()[" + std::to_string(axis) + "]";
        if (type.shape[axis].constant)
          guards +=
              "if(" + actual + "!=" + std::to_string(*type.shape[axis].constant) + ")" + fault;
      }
    }
    if (!guards.empty())
      steps.push_back(std::move(guards));
    std::vector<std::int64_t> conditionalEdges(plan.code.size() + 1, 0);
    for (std::size_t i = 0; i < plan.code.size(); ++i) {
      const auto &op = plan.code[i];
      if (op.code != Code::Branch || op.jump <= i || op.jump >= plan.code.size())
        continue;
      auto end = op.jump;
      if (plan.code[end - 1].code == Code::Jump)
        end = std::max(end, plan.code[end - 1].jump);
      if (end > plan.code.size())
        reject({}, "invalid conditional continuation boundary");
      ++conditionalEdges[i + 1];
      --conditionalEdges[end];
    }
    std::int64_t conditionalDepth = 0;
    for (std::size_t i = 0; i < plan.code.size(); ++i) {
      conditionalDepth += conditionalEdges[i];
      conditionalHost = conditionalDepth > 0;
      const auto &op = plan.code[i];
      plannedPC.push_back(steps.size() + 1);
      const auto span = node(op.source).span;
      std::vector<Value> args;
      for (auto slot : op.args)
        args.push_back(get(slot));
      switch (op.code) {
      case Code::Literal: {
        if (const auto *literal = std::get_if<BoolLiteral>(&node(op.source).operation)) {
          field("bool");
          Value value{Value::Boolean, next - 1};
          steps.push_back(ref(value) + (literal->value ? "=true;\n" : "=false;\n"));
          bind(op.output, value);
          break;
        }
        const auto element = plan.slots.at(op.output).element;
        const auto bits = literalBits(op.source);
        if (!bits)
          reject(span, "expected a typed numeric literal");
        field(scalarType(element));
        Value value{Value::Scalar, next - 1};
        value.element = element;
        steps.push_back(ref(value) + "=" + scalarBits(element, *bits) + ";\n");
        bind(op.output, value);
        break;
      }
      case Code::Unary: {
        if (node(op.source).type.builtin == BuiltinValue::Bool) {
          completed(args.at(0), span);
          if (args.at(0).kind != Value::Boolean)
            reject(span, "Metal host Boolean negation requires Bool");
          field("bool");
          Value value{Value::Boolean, next - 1};
          steps.push_back(ref(value) + "=!" + ref(args.at(0)) + ";\n");
          bind(op.output, value);
          break;
        }
        field("float");
        Value value{Value::Scalar, next - 1};
        const bool neg =
            std::get<ScalarUnary>(node(op.source).operation).operation == TokenKind::Minus;
        steps.push_back(ref(value) + "=std::bit_cast<float>(std::bit_cast<std::uint32_t>(" +
                        ref(args[0]) + ") ^ " + (neg ? "0x80000000u" : "0u") + ");\n");
        bind(op.output, value);
        break;
      }
      case Code::Binary:
        bind(op.output,
             arithmetic(std::get<ScalarBinary>(node(op.source).operation).operation, args, span));
        break;
      case Code::Kernel:
        if (auto found = continuationKernels.find(op.source); found != continuationKernels.end())
          bind(op.output, cpuCall(found->second, args, span, false, false, continuationCpu));
        else
          bind(op.output, numericKernel(op.source, args));
        break;
      case Code::Call:
        bind(op.output, cpuCall(cpuFunction(op.target, span), args, span));
        break;
      case Code::Device: {
        Value value{Value::Target};
        shape(value, plan.slots.at(op.output));
        bind(op.output, value);
        break;
      }
      case Code::Transfer:
      case Code::Spawn: {
        activeGroup = plannedGroups.at(op.group);
        admissionExit = joins.at(op.group);
        if (op.code == Code::Transfer)
          bind(op.output, transfer(args.at(0), args.at(1), span));
        else {
          if (!op.placement)
            reject(span, "mixed spawn requires a checked target");
          auto target = get(*op.placement);
          if (target.gpu)
            gpuCalls[op.output] = op.group;
          const bool closureEntry = target.gpu && std::ranges::any_of(placement.functions.at(op.target).parameters,
              [&](auto symbol) { return containsFunctionStorage(placement, placement.symbols.at(symbol).type); });
          auto errorEntry = deviceErrorEntries.find(op.target);
          bind(op.output, target.gpu && errorEntry != deviceErrorEntries.end()
                              ? spawnNumericProgram(errorEntry->second, args, target, span,
                                    closureEntry ? std::optional<ValueId>{op.source} : std::nullopt, true)
                              : closureEntry
                              ? spawnNumericProgram(op.target, args, target, span, op.source)
                              : resumable.at(op.target)
                                  ? spawnHelper(op.target, args, target, span)
                                  : spawn(cpuFunction(op.target, span), args, target, span));
        }
        break;
      }
      case Code::Open: {
        auto group = plannedGroups.at(op.group);
        std::string reset;
        if (auto join = joins.find(op.group); join != joins.end()) {
          auto output = get(plan.code.at(join->second).output);
          if (output.applicationError)
            reset = ref(applicationError(output)) + "={};\n";
        }
        steps.push_back(reset + group + "=task.openGroup(" +
                        (op.target == SIZE_MAX ? "" : plannedGroups.at(op.target)) + ");\n" +
                        check(group));
        break;
      }
      case Code::Await: {
        auto output = write(op.output, args.at(0)), input = args.at(0);
        if (auto found = gpuCalls.find(op.args[0]); found != gpuCalls.end())
          gpuAwaitSites.push_back({i, found->second});
        std::string read;
        if (input.outcomeHelper) {
          if (!output.applicationError)
            reject(span, "host task application error requires a typed await channel");
          const auto helper = helperName(*input.outcomeHelper);
          const auto family = *placement.functions.at(*input.outcomeHelper).resultType.errorRecord;
          field(recordType(family));
          Value error{Value::Aggregate, next - 1};
          error.record = family;
          read = "alignas(" + helper + "::Outcome) std::byte bytes[sizeof(" + helper +
                 "::Outcome)];\nif(!task.read(" + ref(input) + "," + helper +
                 "::outcomeLayout,bytes))return;\n"
                 "auto &outcome=*std::launder(reinterpret_cast<" + helper + "::Outcome *>(bytes));\n"
                 "const bool failed=outcome.failed;\nif(failed)" + ref(error) +
                 "=outcome.error;else " + ref(output) + "=std::move(outcome.value);\n" +
                 helper + "::outcomeLayout.destroy(&outcome);\nif(failed){\n" +
                 hostErrorCopy(applicationError(output), error) + "}\n";
        } else if (input.outcomeRecord) {
          if (!output.applicationError)
            reject(span, "GPU task application error requires a typed await channel");
          field(recordType(*input.outcomeRecord));
          Value outcome{Value::Aggregate, next - 1};
          outcome.record = input.outcomeRecord;
          const auto &type = placement.records.at(*outcome.record);
          field(recordType(*type.fields.at(2).type.record));
          Value error{Value::Aggregate, next - 1};
          error.record = type.fields.at(2).type.record;
          read = "if(!task.read(" + ref(input) + "," + recordLayoutName(*outcome.record) +
                 ",&" + ref(outcome) + "))return;\nswitch(" + hostErrorTag(outcome) + "){\n" +
                 "case 1:{" + readNativeField(outcome, 1, output) + "break;}\n" +
                 "case 2:{" + readNativeField(outcome, 2, error) +
                 hostErrorCopy(applicationError(output), error) + "break;}\n" +
                 "default:task.fault(3);return;}\n";
        } else if (input.array || input.record)
          read = "if(!task.read(" + ref(input) + "," + arrayLayout(input) + ",&" + ref(output) +
                 ")) return;\n";
        else
          read = (input.booleanTask  ? "if(!readBool(task,"
                  : input.scalarTask ? "if(!readScalar<" + elementName(input.element) + ">(task,"
                                     : "if(!readBuffer(task,") +
                 ref(input) + "," + ref(output) + ")) return;\n";
        steps.push_back("if(!task.await(" + ref(input) + ")) return;\n" + failureRef(output) +
                        "=task.failure(" + ref(input) + ");\n" +
                        ((input.outcomeRecord || input.outcomeHelper) ? ref(output) + "={};\n" : "") +
                        (output.applicationError ? ref(applicationError(output)) + "={};\n" : "") +
                        "if(" + failureRef(output) +
                        ".kind==TaskFailure::None){\n" + read + "}\n" + commitArray(output));
        break;
      }
      case Code::Try:
      case Code::Fail: {
        const auto input = args.at(0);
        const auto target = get(op.target);
        const bool explicitApplication = op.code == Code::Fail && input.record.has_value();
        if (explicitApplication || (op.code == Code::Try && input.applicationError)) {
          if (!target.applicationError)
            reject(span, "GPU application error propagation requires a typed destination");
          const auto error = explicitApplication ? input : applicationError(input);
          std::string code = explicitApplication ? "{\n" : "if(" + hostErrorTag(error) + "!=0){\n";
          if (op.group != SIZE_MAX) {
            code += "if(task.stopGroup(" + plannedGroups.at(op.group) + ")){\n" +
                    hostErrorCopy(applicationError(target), error) + "}\n";
          } else {
            code += failureRef(target) + "={};\n" +
                    hostErrorCopy(applicationError(target), error);
          }
          for (auto collection : op.collections) {
            const auto found = arrayFields.find(get(collection).slot);
            if (found == arrayFields.end())
              reject(span, "missing abandoned collection slot");
            code += "if(!f.arrayScopes.collectDiscard(" +
                    std::to_string(std::distance(arrayFields.begin(), found)) +
                    ")){hostArrayFailure(task);return;}\n";
          }
          steps.push_back(code + jump(op.jump) + "}\n");
          if (explicitApplication)
            break;
        }
        const auto failure = op.code == Code::Fail ? ref(args[0]) : failureRef(args[0]);
        std::string code =
            op.code == Code::Try ? "if(" + failure + ".kind!=TaskFailure::None){\n" : "{\n";
        if (op.group != SIZE_MAX)
          code += "task.failGroup(" + plannedGroups.at(op.group) + "," + failure + ");\n";
        else
          code += failureRef(get(op.target)) + "=" + failure + ";\n";
        for (auto collection : op.collections) {
          const auto value = get(collection);
          const auto found = arrayFields.find(value.slot);
          if (found == arrayFields.end())
            reject(span, "missing abandoned collection slot");
          code += "if(!f.arrayScopes.collectDiscard(" +
                  std::to_string(std::distance(arrayFields.begin(), found)) +
                  ")){hostArrayFailure(task);return;}\n";
        }
        code += jump(op.jump) + "}\n";
        if (op.code == Code::Try)
          code += copy(write(op.output, args[0]), args[0]);
        steps.push_back(std::move(code));
        break;
      }
      case Code::Copy:
        steps.push_back(copy(write(op.output, args.at(0)), args.at(0)));
        break;
      case Code::Catch: {
        auto output = write(op.output, args[0]), error = get(op.args[1]);
        const auto input = args[0];
        const auto app = input.applicationError ? hostErrorTag(applicationError(input)) + "!=0"
                                                : "false";
        const Value failure{Value::Error, *input.failure};
        auto handle = hostErrorCopy(error, failure);
        if (input.applicationError)
          handle = "if(" + app + "){\n" + hostErrorCopy(error, applicationError(input)) +
                   "}else{\n" + handle + "}\n";
        steps.push_back("if(" + failureRef(input) + ".kind==TaskFailure::None && !(" + app +
                        ")){\n" + copy(output, input) + jump(op.target) + "}\n" +
                        handle + jump(op.jump));
        break;
      }
      case Code::Branch:
        completed(args.at(0), span);
        if (args.at(0).kind != Value::Boolean)
          reject(span, "Metal host branch requires Bool");
        steps.push_back("if(!" + ref(args.at(0)) + "){\n" + jump(op.jump) + "}\n");
        break;
      case Code::Case: {
        const auto subject = args.at(0);
        std::string tag;
        if (subject.record) {
          const auto &record = placement.records.at(*subject.record);
          if (record.variants.empty())
            reject(span, "host match requires an enum discriminator");
          const auto offset = aggregateTypes.at(*subject.record).fields.at(0).nativeOffset;
          tag = "([&](){std::int64_t tag=0;std::memcpy(&tag," + ref(subject) + ".data.data()+" +
                std::to_string(offset) + ",sizeof(tag));if(tag<1 || tag>" +
                std::to_string(record.variants.size()) +
                "){task.context().fault(3);return std::int64_t(0);}return tag;}())";
        } else
          tag = subject.kind == Value::Error ? "unsigned(" + ref(subject) + ".kind)" : ref(subject);
        steps.push_back(
            "auto caseTag=" + tag +
            ";if(task.context().status()){hostArrayFailure(task);return;}\nif(caseTag!=" +
            std::to_string(op.target) + "){\n" +
            (op.jump == SIZE_MAX ? error("m::Error::Invalid") : jump(op.jump)) + "}\n");
        break;
      }
      case Code::Jump:
        if (op.jump <= i && std::holds_alternative<WhileOperation>(node(op.source).operation))
          steps.push_back("if(task.context().poll()){hostArrayFailure(task);return;}\n"
                          "if(task.cancelled())return;\n");
        steps.push_back(jump(op.jump));
        break;
      case Code::Field: {
        auto output = get(op.output);
        auto subject = args.at(0);
        if (std::holds_alternative<ErrorNarrow>(node(op.source).operation)) {
          steps.push_back(hostErrorCopy(output, subject));
          break;
        }
        if (const auto *read = std::get_if<RecordFieldRead>(&node(op.source).operation)) {
          steps.push_back(readNativeField(subject, read->field, output) + commitArray(output));
          break;
        }
        std::string prefix;
        if (const auto *read = std::get_if<ErrorFieldRead>(&node(op.source).operation);
            read && read->taskSlot) {
          field("Failure");
          const Value failure{Value::Error, next - 1};
          prefix = readNativeField(subject, *read->taskSlot, failure);
          subject = failure;
        }
        auto field = static_cast<ErrorField>(op.target);
        std::string code;
        if (field == ErrorField::Kind)
          code = ref(subject) + ".code==1 ? 3u : " + ref(subject) +
                 ".code==2 ? 4u : " + ref(subject) + ".code==4 ? 1u : " + ref(subject) +
                 ".code==5 ? 2u : 5u";
        else if (field == ErrorField::Message)
          code = ref(subject) + ".code==1 ? \"tensor shape mismatch\" : " + ref(subject) +
                 ".code==2 ? \"allocation failed\" : " + ref(subject) +
                 ".code==4 ? \"integer arithmetic fault\" : " + ref(subject) +
                 ".code==5 ? \"index out of bounds\" : \"native execution fault\"";
        else
          code = "\"task admission rejected\"";
        steps.push_back(prefix + ref(output) + "=" + code + ";\n");
        break;
      }
      case Code::Join: {
        const auto group = plannedGroups.at(op.group);
        auto output = write(op.output, args.at(0));
        steps.push_back(check("task.close(" + group + ")"));
        if (output.applicationError) {
          const auto error = applicationError(output);
          steps.push_back("if(!task.join(" + group + "))return;\nif(" +
                          hostErrorTag(error) + "==0){\n" + failureRef(output) +
                          "=task.failure(" + group + ");\nif(" + failureRef(output) +
                          ".kind==TaskFailure::None){\n" + copy(output, args.at(0)) +
                          "}}else{" + failureRef(output) + "={};" + ref(output) + "={};\n" +
                          commitArray(output) + "}\n");
          break;
        }
        steps.push_back("if(!task.join(" + group + ")) return;\n" + failureRef(output) +
                        "=task.failure(" + group + ");\nif(" + failureRef(output) +
                        ".kind==TaskFailure::None){\n" + payloadCopy(output, args.at(0)) + "}\n" +
                        commitArray(output));
        break;
      }
      case Code::PropagateGroup:
        steps.push_back("{auto failure=task.failure(" + plannedGroups.at(op.group) +
                        ");if(failure.kind!=TaskFailure::None){task.fail(failure);return;}}\n");
        break;
      case Code::CollectInit:
      case Code::CollectAppend:
      case Code::CollectFinish: {
        const auto output = get(op.output);
        const auto found = arrayFields.find(output.slot);
        if (found == arrayFields.end())
          reject(span, "missing collection ownership slot");
        const auto index = std::to_string(std::distance(arrayFields.begin(), found));
        std::string call;
        if (op.code == Code::CollectInit)
          call = "f.arrayScopes.collectBegin(" + index + ",hostScalarLayout<" +
                 elementName(output.element) + "," + (output.booleanArray ? "true" : "false") +
                 ">()," + arrayLayout(output) + "," + (output.booleanArray ? "true" : "false") +
                 ")";
        else if (op.code == Code::CollectAppend)
          call = "f.arrayScopes.collectAppend(" + index + ",&" + ref(args.at(0)) + ")";
        else
          call = "f.arrayScopes.collectFinish(" + index + ",&" + ref(output) + ")";
        steps.push_back("if(!" + call + "){hostArrayFailure(task);return;}\n" +
                        (op.code == Code::CollectFinish ? commitArray(output) : ""));
        break;
      }
      case Code::MapInit:
      case Code::MapPrepare:
      case Code::MapJoin:
      case Code::MapPublish:
      case Code::MapCheck:
      case Code::MapLoad:
      case Code::MapStore:
      case Code::MapAdvance:
      case Code::MapSuccess:
      case Code::CallableTarget:
      case Code::CallableCaptures:
      case Code::CallableSyncGuard:
      case Code::InvalidCallable:
      case Code::FunctionReference:
      case Code::FunctionInvoke:
      case Code::SynchronousCall:
        reject(span, "function values require the CPU backend");
      case Code::ReleaseGroup:
        steps.push_back(check("task.releaseGroup(" + plannedGroups.at(op.group) + ")"));
        break;
      case Code::Label:
        break;
      case Code::Return: {
        auto result = args.at(0);
        auto code = (result.failure
                             ? "if(" + failureRef(result) + ".kind!=TaskFailure::None){task.fail(" +
                                   failureRef(result) + ");return;}\n"
                             : "");
        if (result.applicationError) {
          const auto error = applicationError(result);
          code += std::string(publishApplicationOutcome ? "MainOutcome" : "Outcome") +
                  " outcome(" + hostErrorTag(error) + "!=0);\n"
                  "if(outcome.failed)std::construct_at(&outcome.error," + ref(error) +
                  ");else std::construct_at(&outcome.value," + ref(result) + ");\n"
                  "task.complete(&outcome);\n"
                  "if(outcome.failed)std::destroy_at(&outcome.error);else std::destroy_at(&outcome.value);\n"
                  "return;\n";
        } else
          code += "task.complete(&" + ref(result) + ");return;\n";
        steps.push_back(std::move(code));
        break;
      }
      }
    }
    // All edges address common plan instructions; close/join may use two target
    // steps.
    for (auto &step : steps)
      for (std::size_t pos = 0; (pos = step.find("@pc", pos)) != std::string::npos;) {
        const auto end = step.find('@', pos + 3);
        const auto index = std::stoull(step.substr(pos + 3, end - pos - 3));
        step.replace(pos, end - pos + 1, std::to_string(plannedPC.at(index)));
      }
    return get(plan.result);
  }
  std::string helperSuccessLayout(const ValueType &type) const {
    if (type.array || type.record)
      return arrayLayout(type);
    if (type.tensor)
      return "m::Runtime::bufferLayout()";
    if (type.builtin == BuiltinValue::Bool)
      return "boolScalarLayout()";
    return "(*numericScalarLayout(" + elementName(type.element) + "))";
  }
  std::string outcomeDeclaration(const std::string &type, const std::string &layout,
                                 const std::string &payload, std::size_t family,
                                 const std::string &success, std::uint64_t identity) const {
    const auto error = recordLayoutName(family);
    return "struct " + type + " {bool failed;union {" + payload + " value;" +
           recordName(family) + " error;};\nexplicit " + type +
           "(bool failed) noexcept:failed(failed){}\n~" + type + "() noexcept {}\n};\n"
           "const ValueLayout " + layout + "{" + std::to_string(identity) + "ull,sizeof(" + type +
           "),alignof(" + type + "),\n"
           "+[](Invocation &dst,void *out,const Invocation &src,const void *in) noexcept {\n"
           "const auto &from=*static_cast<const " + type + " *>(in);\n"
           "auto &to=*new(out) " + type + "(from.failed);\n"
           "const bool copied=from.failed ? " + error + ".copy(dst,&to.error,src,&from.error) : " +
           success + ".copy(dst,&to.value,src,&from.value);\nif(!copied)to.~" + type +
           "();return copied;},\n+[](void *raw) noexcept {\n"
           "auto &value=*static_cast<" + type + " *>(raw);\n"
           "if(value.failed)" + error + ".destroy(&value.error);else " + success +
           ".destroy(&value.value);\nvalue.~" + type + "();}};\n";
  }
  std::string helperPacket(FunctionId id) const {
    std::string code = "namespace " + helperName(id) +
                       " { struct PacketData { m::Runtime *runtime; const char *directory;";
    const auto &fn = placement.functions.at(id);
    for (std::size_t n = 0; n < fn.parameters.size(); ++n) {
      const auto &type = placement.symbols.at(fn.parameters[n]).type;
      code +=
          (type.record  ? recordName(*type.record)
           : type.array ? tensorDescriptor(isBoolArray(type) ? NumericType::UInt8 : type.element, 1)
           : type.tensor                        ? "m::Buffer"
           : type.builtin == BuiltinValue::Bool ? "bool"
                                                : scalarType(type.element)) +
          " arg" + std::to_string(n) + ";";
    }
    code += "}; extern const TaskEntry entry;\n";
    if (const auto family = fn.resultType.errorRecord) {
      const auto &type = fn.resultType;
      const auto payload = type.record ? recordName(*type.record)
          : type.array ? tensorDescriptor(isBoolArray(type) ? NumericType::UInt8 : type.element, 1)
          : type.tensor ? std::string("m::Buffer")
          : type.builtin == BuiltinValue::Bool ? std::string("bool") : scalarType(type.element);
      code += outcomeDeclaration("Outcome", "outcomeLayout", payload, *family,
                                 helperSuccessLayout(type), 0x4b414c484f000000ULL + id);
    }
    return code + "}\n";
  }
  std::string helperBody(FunctionId id) const {
    const auto &fn = placement.functions.at(id);
    std::ostringstream out;
    out << "namespace " << helperName(id) << " {\n"
        << arrayScopeDeclaration()
        << "struct Frame {m::Runtime *runtime; std::string directory; "
           "unsigned pc=1;\n";
    for (const auto &field : fields)
      out << field << '\n';
    if (!arrayFields.empty())
      out << "HostArrayScopes arrayScopes{};\n";
    out << "};\nbool construct(Invocation &context,void *out,const Packet "
           "&packet){\n"
           "const auto &data=*static_cast<const PacketData*>(packet.data);\n"
           "if(!data.runtime || !data.directory";
    for (std::size_t n = 0; n < plannedParameters.size(); ++n) {
      const auto &type = placement.symbols.at(fn.parameters[n]).type;
      if (type.tensor && !type.array) {
        auto arg = "data.arg" + std::to_string(n);
        out << " || !" << arg << " || " << arg << ".onDevice() || " << arg
            << ".format()!=*m::storageFormat(" << elementName(type.element) << ") || " << arg
            << ".shape().size()!=" << type.shape.size();
      }
    }
    const bool arrayParameters = std::ranges::any_of(plannedParameters, [](const auto &v) {
      return v.kind == Value::Array || v.kind == Value::Aggregate || v.kind == Value::Callable;
    });
    if (arrayParameters)
      out << " || !packet.source";
    out << "){context.fault(3);return false;}\n"
           "auto &f=*new(out) Frame{data.runtime,data.directory}; (void)f;\n";
    if (!arrayFields.empty())
      out << "if(!f.arrayScopes.initialize(context,arrayLayouts)){f.~Frame();return false;}\n";
    for (std::size_t n = 0; n < plannedParameters.size(); ++n) {
      const auto &value = plannedParameters[n];
      if (value.kind == Value::Array || value.kind == Value::Aggregate ||
          value.kind == Value::Callable)
        out << "if(!f.arrayScopes.capture(context,"
            << std::distance(arrayFields.begin(), arrayFields.find(value.slot)) << ","
            << arrayLayout(value) << ",&" << ref(value) << ",*packet.source,&data.arg" << n
            << ")){f.~Frame();return false;}\n";
      else
        out << ref(value) << "=data.arg" << n << ";\n";
    }
    out << "return true;}\nvoid destroy(void *p) noexcept "
           "{static_cast<Frame*>(p)->~Frame();}\n"
           "void resume(Task &task){auto "
           "&f=*static_cast<Frame*>(task.frame()); auto "
           "&r=*f.runtime; (void)r;\n"
           "if(task.cancelled())return;\nfor(;;){switch(f.pc){\n";
    for (std::size_t i = 0; i < steps.size(); ++i)
      out << "case " << i + 1 << ": {\n"
          << (arrayFields.empty()
                  ? ""
                  : "HostArrayWork work(f.arrayScopes);if(!work){hostArrayFailure(task);return;}\n")
          << steps[i] << "f.pc=" << i + 2 << ";\n} [[fallthrough]];\n";
    out << "default:task.fault(3);return;}}}\n"
           "const TaskEntry entry{1,"
        << 0x4b414c52534d0000ULL + id
        << "ull,sizeof(PacketData),alignof(PacketData),"
           "sizeof(Frame),alignof(Frame),construct,destroy,resume,"
        << (fn.resultType.errorRecord ? "&outcomeLayout" : "&" + helperSuccessLayout(fn.resultType))
        << "};\n}\n";
    return out.str();
  }
  static std::string reductionShader() {
    return R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @kalandra_reduce_sum(%input: memref<?xf32>, %output: memref<?xf32>, %parameters: memref<1xi32>)
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %i = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %one = arith.constant 1 : index
      %two = arith.constant 2 : index
      %count = memref.load %parameters[%zero] : memref<1xi32>
      %n = arith.index_cast %count : i32 to index
      %left = arith.muli %i, %two : index
      %valid = arith.cmpi ult, %left, %n : index
      scf.if %valid {
        %a = memref.load %input[%left] : memref<?xf32>
        %right = arith.addi %left, %one : index
        %paired = arith.cmpi ult, %right, %n : index
        %result = scf.if %paired -> f32 {
          %b = memref.load %input[%right] : memref<?xf32>
          %sum = arith.addf %a, %b : f32
          scf.yield %sum : f32
        } else {
          scf.yield %a : f32
        }
        memref.store %result, %output[%i] : memref<?xf32>
      }
      gpu.return
    }
  }
}
)";
  }
  std::string shader(std::size_t function, std::vector<std::uint32_t> *readInputs = nullptr,
                     bool post = false, int matrixOperand = -1, bool finalScalar = false,
                     int graphStage = -1) const {
    const auto &graph = matrixGraph(function);
    std::size_t graphIndex = 0;
    std::size_t reductionIndex = 0;
    const bool multi = graph && (graph->reductions.size() >= 2 || graph->tensorResult);
    std::optional<std::string> graphResult;
    if (graph && !post && graphStage < 0)
      return graph->matrixCount ? matmulShader : reductionShader();
    const bool matrixPost = post && matrixFunction(function) && !finalScalar;
    if (!post && matrixOperand < 0 && matrixFunction(function))
      return matmulShader;
    if (!post && !matrixFunction(function) && reduction(function) &&
        !primitiveEntry(function)->mapped)
      return reductionShader();
    // Emit scalar typed SSA once per lane. Source names never become IR
    // identifiers.
    struct Instruction {
      std::string result, code;
      std::vector<std::string> operands;
    };
    std::vector<Instruction> instructions;
    std::unordered_map<std::string, std::size_t> definitions;
    std::unordered_map<std::string, std::uint32_t> tensorInputs;
    // SSA origins track the source budget while nested arms lower lane by lane.
    std::unordered_map<std::string, std::set<std::string>> origins;
    std::size_t laneDepth = 0;
    std::size_t counter = 0, callDepth = 0, shapeWork = 0, sourceWork = 0;
    std::optional<std::string> reductionInput, matrixInput;
    std::set<std::size_t> active;
    auto temporary = [&] {
      if (counter >= 100000)
        reject({}, "GPU IR expansion limit exceeded");
      return "%v" + std::to_string(counter++);
    };
    std::function<std::string(std::size_t, const std::vector<std::string> &)> invoke;
    invoke = [&](std::size_t fnId, const std::vector<std::string> &args) -> std::string {
      const auto &fn = cpu.functions.at(fnId);
      for (auto parameter : fn.parameters)
        if (cpu.symbols[parameter].type.tensor && (cpu.symbols[parameter].type.shape.empty() ||
                                                   cpu.symbols[parameter].type.shape.size() > 8))
          reject(fn.declaration, "elementwise GPU helpers require rank-one "
                                 "through rank-eight tensors");
      if (fn.resultType.tensor && (fn.resultType.shape.empty() || fn.resultType.shape.size() > 8))
        reject(fn.declaration, "elementwise GPU helpers require rank-one "
                               "through rank-eight results");
      if (callDepth >= 128 || !active.insert(fnId).second)
        reject(fn.declaration, "recursive GPU helper calls are not implemented");
      Depth guard(callDepth);
      // Per-invocation equivalence classes start with unconditional checked
      // contracts. Choice-result aliases stay local: an aliased call must not
      // prove a later call safe.
      using ExtentKey = std::pair<bool, std::uint64_t>;
      std::map<ExtentKey, ExtentKey> parents;
      std::map<ExtentKey, unsigned> ranks;
      auto key = [](const Extent &extent) -> ExtentKey {
        return {extent.constant.has_value(), extent.constant.value_or(extent.identity)};
      };
      auto root = [&](ExtentKey value) {
        auto result = value;
        while (true) {
          if (++shapeWork > 300000)
            reject(fn.declaration, "GPU shape proof analysis limit exceeded");
          auto [found, inserted] = parents.emplace(result, result);
          if (inserted || found->second == result)
            break;
          result = found->second;
        }
        while (value != result) {
          auto next = parents.at(value);
          parents[value] = result;
          value = next;
        }
        return result;
      };
      auto validExtent = [](const Extent &extent) {
        return extent.constant.has_value() || extent.identity != 0;
      };
      auto unite = [&](const Extent &a, const Extent &b) {
        if (!validExtent(a) || !validExtent(b))
          return;
        auto left = root(key(a)), right = root(key(b));
        if (left == right)
          return;
        if (ranks[left] < ranks[right])
          std::swap(left, right);
        parents[right] = left;
        if (ranks[left] == ranks[right])
          ++ranks[left];
      };
      bool initializedShapes = false;
      auto sameExtent = [&](const Extent &a, const Extent &b) {
        if (!initializedShapes) {
          initializedShapes = true;
          for (const auto &requirement : fn.preconditions)
            unite(requirement.left, requirement.right);
        }
        return validExtent(a) && validExtent(b) && root(key(a)) == root(key(b));
      };
      std::unordered_map<SymbolId, std::string> symbols;
      std::unordered_map<ValueId, std::string> values;
      for (std::size_t i = 0; i < args.size(); ++i)
        symbols[fn.parameters.at(i)] = args[i];
      std::function<std::string(ValueId)> emit = [&](ValueId id) -> std::string {
        if (auto found = values.find(id); found != values.end())
          return found->second;
        const auto &n = cpu.expressions.at(id);
        std::string result;
        std::ostringstream instruction;
        std::vector<std::string> operands;
        if (auto *read = std::get_if<SymbolRead>(&n.operation))
          result = symbols.at(read->symbol);
        else if (auto *literal = std::get_if<BoolLiteral>(&n.operation)) {
          result = temporary();
          instruction << "        " << result << " = arith.constant "
                      << (literal->value ? "true" : "false") << "\n";
        } else if (const auto *compare = std::get_if<ScalarCompare>(&n.operation)) {
          auto a = emit(compare->left), b = emit(compare->right);
          operands = {a, b};
          const auto &type = cpu.expressions.at(compare->left).type;
          if (type.builtin == BuiltinValue::Bool) {
            if (compare->operation != ComparisonKind::Equal &&
                compare->operation != ComparisonKind::NotEqual)
              reject(n.span, "GPU Boolean comparisons support only == and !=");
            result = temporary();
            instruction << "        " << result << " = arith.cmpi "
                        << (compare->operation == ComparisonKind::Equal ? "eq" : "ne") << ", " << a
                        << ", " << b << " : i1\n";
          } else {
            if (type.tensor || type.builtin != BuiltinValue::Numeric ||
                type.element != NumericType::Float32)
              reject(n.span, "GPU comparisons require float32 scalar operands");
            auto op = [&](const std::string &code) {
              auto v = temporary();
              instruction << "        " << v << " = arith." << code << "\n";
              return v;
            };
            auto ai = op("bitcast " + a + " : f32 to i32"),
                 bi = op("bitcast " + b + " : f32 to i32");
            auto mask = op("constant 2147483647 : i32"), inf = op("constant 2139095040 : i32"),
                 zero = op("constant 0 : i32"), sign = op("constant -2147483648 : i32"),
                 yes = op("constant true");
            auto both = [&](std::string x, std::string y) {
              return op("andi " + x + ", " + y + " : i1");
            };
            auto either = [&](std::string x, std::string y) {
              return op("ori " + x + ", " + y + " : i1");
            };
            auto negate = [&](std::string x) { return op("xori " + x + ", " + yes + " : i1"); };
            auto cmp = [&](std::string pred, std::string x, std::string y) {
              return op("cmpi " + pred + ", " + x + ", " + y + " : i32");
            };
            auto ma = op("andi " + ai + ", " + mask + " : i32"),
                 mb = op("andi " + bi + ", " + mask + " : i32");
            auto ordered = both(cmp("ule", ma, inf), cmp("ule", mb, inf));
            auto equal = either(cmp("eq", ai, bi), both(cmp("eq", ma, zero), cmp("eq", mb, zero)));
            auto sa = op("andi " + ai + ", " + sign + " : i32"),
                 sb = op("andi " + bi + ", " + sign + " : i32");
            auto negative = cmp("ne", sa, zero);
            auto same = op("select " + negative + ", " + cmp("ugt", ai, bi) + ", " +
                           cmp("ult", ai, bi) + " : i1");
            auto less = op("select " + cmp("ne", sa, sb) + ", " + negative + ", " + same + " : i1");
            less = both(negate(equal), less);
            switch (compare->operation) {
            case ComparisonKind::Equal:
              result = both(ordered, equal);
              break;
            case ComparisonKind::NotEqual:
              result = negate(both(ordered, equal));
              break;
            case ComparisonKind::Less:
              result = both(ordered, less);
              break;
            case ComparisonKind::LessEqual:
              result = both(ordered, either(less, equal));
              break;
            case ComparisonKind::Greater:
              result = both(ordered, negate(either(less, equal)));
              break;
            case ComparisonKind::GreaterEqual:
              result = both(ordered, negate(less));
              break;
            }
          }
        } else if (const auto *branch = std::get_if<ConditionalOperation>(&n.operation)) {
          const bool tensor = n.type.tensor;
          if (!(tensor ? pureLane(branch->trueResult) && pureLane(branch->falseResult)
                       : pureScalar(branch->trueResult) && pureScalar(branch->falseResult)))
            reject(n.span, "GPU choices require pure scalar or shape-proven "
                           "elementwise arms; "
                           "primitives, shape checks and dispatch are not "
                           "supported in arms");
          auto condition = emit(branch->condition);
          if (tensor)
            ++laneDepth;
          auto saved = symbols;
          auto arm = [&](const std::vector<TypedStatement> &body, ValueId id) {
            for (const auto &statement : body) {
              if (!(tensor ? pureLane(statement.value) : pureScalar(statement.value)))
                reject(n.span, "GPU branch bodies require pure lane calculations");
              auto value = emit(statement.value);
              if (statement.binding)
                symbols[*statement.binding] = value;
            }
            return emit(id);
          };
          auto a = arm(branch->whenTrue, branch->trueResult);
          symbols = saved;
          auto b = arm(branch->whenFalse, branch->falseResult);
          symbols = std::move(saved);
          if (tensor) {
            --laneDepth;
            auto sources = origins[a];
            sources.insert(origins[b].begin(), origins[b].end());
            sourceWork += sources.size();
            if (sources.empty() || sourceWork > 300000)
              reject(n.span, "GPU fused tensor choice operand analysis limit exceeded");
            const auto &left = cpu.expressions.at(branch->trueResult).type;
            const auto &right = cpu.expressions.at(branch->falseResult).type;
            if (left.shape.size() != right.shape.size())
              reject(n.span, "GPU tensor choice ranks differ");
            for (std::size_t axis = 0; axis < left.shape.size(); ++axis) {
              if (sources.size() > 1 && !sameExtent(left.shape[axis], right.shape[axis]))
                reject(n.span, "different-source GPU choices require proven equal shapes");
              // The merged type may carry a fresh extent even though both
              // results agree.
              unite(n.type.shape[axis], left.shape[axis]);
              unite(left.shape[axis], right.shape[axis]);
            }
          }
          result = temporary();
          if (tensor) {
            origins[result] = origins[a];
            origins[result].insert(origins[b].begin(), origins[b].end());
          }
          operands = {condition, a, b};
          if (n.type.builtin == BuiltinValue::Bool)
            instruction << "        " << result << " = arith.select " << condition << ", " << a
                        << ", " << b << " : i1\n";
          else {
            // Select bits, preserving signed zeros/subnormals/NaN payloads
            // without introducing an unqualified native floating-point
            // operation.
            auto ai = temporary(), bi = temporary(), selected = temporary();
            instruction << "        " << ai << " = arith.bitcast " << a << " : f32 to i32\n"
                        << "        " << bi << " = arith.bitcast " << b << " : f32 to i32\n"
                        << "        " << selected << " = arith.select " << condition << ", " << ai
                        << ", " << bi << " : i32\n"
                        << "        " << result << " = arith.bitcast " << selected
                        << " : i32 to f32\n";
          }
        } else if (auto *literal = std::get_if<Float32Literal>(&n.operation)) {
          auto integer = temporary();
          result = temporary();
          instruction << "        " << integer << " = arith.constant "
                      << std::int32_t(literal->bits) << " : i32\n";
          instruction << "        " << result << " = arith.bitcast " << integer
                      << " : i32 to f32\n";
        } else if (std::holds_alternative<ScalarBinary>(n.operation) ||
                   std::holds_alternative<TensorBinary>(n.operation)) {
          auto operation = std::holds_alternative<ScalarBinary>(n.operation)
                               ? std::get<ScalarBinary>(n.operation).operation
                               : std::get<TensorBinary>(n.operation).operation;
          auto left = std::holds_alternative<ScalarBinary>(n.operation)
                          ? std::get<ScalarBinary>(n.operation).left
                          : std::get<TensorBinary>(n.operation).left;
          auto right = std::holds_alternative<ScalarBinary>(n.operation)
                           ? std::get<ScalarBinary>(n.operation).right
                           : std::get<TensorBinary>(n.operation).right;

          if (operation != TokenKind::Plus && operation != TokenKind::Minus &&
              operation != TokenKind::Star && operation != TokenKind::Slash)
            reject(n.span, "GPU strict arithmetic currently supports only +, -, * and /");
          auto a = emit(left), b = emit(right);
          const auto &leftType = cpu.expressions.at(left).type;
          const auto &rightType = cpu.expressions.at(right).type;
          if (laneDepth && leftType.tensor && rightType.tensor) {
            for (std::size_t axis = 0; axis < leftType.shape.size(); ++axis) {
              const bool commonSource = origins[a].size() == 1 && origins[a] == origins[b];
              if (!commonSource && !sameExtent(leftType.shape[axis], rightType.shape[axis]))
                reject(n.span, "GPU branch arithmetic requires already-proven "
                               "equal shapes");
              unite(leftType.shape[axis], rightType.shape[axis]);
              unite(n.type.shape[axis], leftType.shape[axis]);
            }
          }
          result = temporary();
          operands = {a, b};
          instruction << "        " << result << " = arith."
                      << (operation == TokenKind::Plus    ? "addf "
                          : operation == TokenKind::Minus ? "subf "
                          : operation == TokenKind::Slash ? "divf "
                                                          : "mulf ")
                      << a << ", " << b << " : f32\n";
        } else if (std::holds_alternative<ScalarUnary>(n.operation) ||
                   std::holds_alternative<TensorUnary>(n.operation)) {
          const auto op = std::holds_alternative<ScalarUnary>(n.operation)
                              ? std::get<ScalarUnary>(n.operation).operation
                              : std::get<TensorUnary>(n.operation).operation;
          const auto input = std::holds_alternative<ScalarUnary>(n.operation)
                                 ? std::get<ScalarUnary>(n.operation).operand
                                 : std::get<TensorUnary>(n.operation).operand;
          result = emit(input);
          if (op == TokenKind::Minus) {
            auto operand = result;
            result = temporary();
            operands = {operand};
            instruction << "        " << result << " = arith.negf " << operand << " : f32\n";
          } else if (op == TokenKind::Relu) {
            auto operand = result;
            operands = {operand};
            auto bits = temporary(), zero = temporary(), mask = temporary(), infinity = temporary();
            auto magnitude = temporary(), negative = temporary(), notNaN = temporary(),
                 clear = temporary(), selected = temporary();
            result = temporary();
            instruction << "        " << bits << " = arith.bitcast " << operand << " : f32 to i32\n"
                        << "        " << zero << " = arith.constant 0 : i32\n"
                        << "        " << mask << " = arith.constant 2147483647 : i32\n"
                        << "        " << infinity << " = arith.constant 2139095040 : i32\n"
                        << "        " << magnitude << " = arith.andi " << bits << ", " << mask
                        << " : i32\n"
                        << "        " << negative << " = arith.cmpi slt, " << bits << ", " << zero
                        << " : i32\n"
                        << "        " << notNaN << " = arith.cmpi ule, " << magnitude << ", "
                        << infinity << " : i32\n"
                        << "        " << clear << " = arith.andi " << negative << ", " << notNaN
                        << " : i1\n"
                        << "        " << selected << " = arith.select " << clear << ", " << zero
                        << ", " << bits << " : i32\n"
                        << "        " << result << " = arith.bitcast " << selected
                        << " : i32 to f32\n";
          } else if (op != TokenKind::Plus)
            reject(n.span, "GPU unary arithmetic is not implemented");
        } else if (auto *sum = std::get_if<ReduceSum>(&n.operation)) {
          // The bounded proof permits exactly one reduction. Before it, emit
          // its tensor input; after it, read the already-reduced device scalar.
          if (!reduction(function) && !(matrixGraph(function) && matrixGraph(function)->reduce))
            reject(n.span, "unexpected reduction in elementwise shader");
          if (multi) {
            emit(sum->input);
            result = "%lane" + std::to_string(post ? reductionIndex++
                                                   : graph->inputs + graph->products.size() +
                                                         reductionIndex++);
          } else
            result = post && !matrixPost ? "%lane0" : emit(sum->input);
          reductionInput = result;
        } else if (const auto *matrix = std::get_if<Matmul>(&n.operation); matrix && graph) {
          emit(matrix->left);
          emit(matrix->right);
          result = std::string(post && multi ? "%graph" : "%lane") +
                   std::to_string(graph->inputs + graphIndex++);
        } else if (const auto *matrix = std::get_if<Matmul>(&n.operation);
                   matrix && (matrixPost || matrixOperand >= 0 || finalScalar)) {
          if (matrixOperand >= 0)
            matrixInput = emit(matrixOperand == 0 ? matrix->left : matrix->right);
          result = matrixPost ? "%lane0" : "%matrix";
          // The primitive is a stage boundary. Only the selected operand (or
          // the completed matrix) survives scalar SSA liveness below.
        } else if (auto *check = std::get_if<ShapeCheck>(&n.operation)) {
          // Straight-line helper checks become the entry's transitive shape
          // preconditions, validated on the host before kernel admission.
          result = emit(check->input);
        } else if (auto *call = std::get_if<FunctionCall>(&n.operation)) {
          std::vector<std::string> inputs;
          for (auto arg : call->arguments)
            inputs.push_back(emit(arg));
          result = invoke(call->function, inputs);
        } else
          reject(n.span, "unsupported GPU operation in a transitive helper");
        if (n.type.tensor) {
          if (laneDepth && (std::holds_alternative<TensorBinary>(n.operation) ||
                            std::holds_alternative<TensorUnary>(n.operation))) {
            auto &sources = origins[result];
            for (const auto &operand : operands)
              if (auto found = origins.find(operand); found != origins.end())
                sources.insert(found->second.begin(), found->second.end());
            sourceWork += sources.size();
            if (sources.empty() || sourceWork > 300000)
              reject(n.span, "GPU fused tensor arithmetic operand analysis "
                             "limit exceeded");
          } else if (!origins.contains(result) || !laneDepth)
            origins[result] = {result};
        }
        if (graph && !laneDepth &&
            (std::holds_alternative<TensorBinary>(n.operation) ||
             std::holds_alternative<TensorUnary>(n.operation) ||
             (std::holds_alternative<ConditionalOperation>(n.operation) && n.type.tensor))) {
          auto stage = graphIndex++;
          if (std::int64_t(stage) == graphStage)
            graphResult = result;
          else {
            result = std::string(post && multi ? "%graph" : "%lane") +
                     std::to_string(graph->inputs + stage);
            origins[result] = {result};
            instruction.str("");
          }
        }
        auto emitted = instruction.str();
        if (!emitted.empty()) {
          definitions.emplace(result, instructions.size());
          instructions.push_back({result, std::move(emitted), std::move(operands)});
        }
        values.emplace(id, result);
        return result;
      };
      for (const auto &statement : fn.body) {
        auto value = emit(statement.value);
        if (statement.binding)
          symbols[*statement.binding] = value;
      }
      auto result = emit(fn.result);
      active.erase(fnId);
      return result;
    };
    std::vector<std::string> args;
    unsigned tensors = 0, scalars = 0;
    for (auto parameter : cpu.functions[function].parameters) {
      if (cpu.symbols[parameter].type.tensor) {
        auto lane = std::string(post && !matrixPost ? "%source" : "%lane") +
                    std::to_string(tensors + (matrixPost ? 1 : 0));
        if (!post || matrixPost)
          tensorInputs.emplace(lane, tensors + (matrixPost ? 1 : 0));
        ++tensors;
        origins[lane] = {lane};
        args.push_back(lane);
      } else
        args.push_back("%scalar" + std::to_string(scalars++));
    }
    auto result = invoke(function, args);
    if (post) {
      tensors = multi ? graph->reductions.size() : matrixPost ? tensors + 1 : 1;
      for (unsigned i = 0; i < (multi ? tensors : 1); ++i)
        tensorInputs.emplace("%lane" + std::to_string(i), i);
      if (matrixPost && reduction(function)) {
        if (!reductionInput)
          reject({}, "missing GPU matrix reduction input");
        result = *reductionInput;
      }
    } else if (matrixOperand >= 0) {
      if (!matrixInput)
        reject({}, "missing GPU matrix input");
      result = *matrixInput;
    } else if (reduction(function)) {
      if (!reductionInput)
        reject({}, "missing GPU reduction input");
      result = *reductionInput;
    }
    if (graphStage >= 0) {
      if (!graphResult)
        reject({}, "missing GPU graph arithmetic stage");
      result = *graphResult;
      for (std::size_t i = 0; i < graph->products.size(); ++i)
        tensorInputs.emplace("%lane" + std::to_string(graph->inputs + i), graph->inputs + i);
      for (std::size_t i = 0; i < graph->reductions.size(); ++i) {
        const auto id = graph->inputs + graph->products.size() + i;
        tensorInputs.emplace("%lane" + std::to_string(id), id);
      }
    }
    std::set<std::uint32_t> used;
    std::set<std::string> live;
    // Iterative traversal bounds compiler stack even for a long left-associated
    // expression.
    std::vector<std::string> pending{result};
    while (!pending.empty()) {
      auto value = std::move(pending.back());
      pending.pop_back();
      if (!live.insert(value).second)
        continue;
      if (auto input = tensorInputs.find(value); input != tensorInputs.end())
        used.insert(input->second);
      else if (auto definition = definitions.find(value); definition != definitions.end())
        for (const auto &operand : instructions[definition->second].operands)
          pending.push_back(operand);
    }
    if (!kalandra::runtime::metal::indirectElementwiseLayout(used.size(), scalars))
      reject(cpu.functions[function].declaration,
             "GPU argument binding representation limit exceeded");
    if (readInputs)
      readInputs->assign(used.begin(), used.end());
    std::ostringstream code;
    for (const auto &instruction : instructions)
      if (live.contains(instruction.result))
        code << instruction.code;
    if (graphStage >= 0)
      tensors = used.size();
    const bool indirect = (graph && scalarControl(function)) ||
                          retainedChoiceBindings(function, used.size()) ||
                          !kalandra::runtime::metal::directElementwiseLayout(used.size(), scalars);
    const bool legacy = !graph && !reduction(function) && !indirect && tensors == 2 &&
                        scalars == 0 && used.size() == tensors;
    auto parameters = "memref<" + std::to_string(legacy ? 2 : scalars + 1) + "xi32>";
    std::string bindings, loads;
    for (auto input : used) {
      auto buffer = "%a" + std::to_string(input), lane = "%lane" + std::to_string(input);
      bindings += buffer + ": memref<?xf32>, ";
      const bool broadcast = graphStage >= 0 && input >= graph->inputs + graph->products.size();
      loads += "        " + lane + " = memref.load " + buffer + (broadcast ? "[%zero]" : "[%i]") +
               " : memref<?xf32>\n";
    }
    for (unsigned i = 0; i < scalars; ++i) {
      auto suffix = std::to_string(i);
      loads +=
          "        %index" + suffix + " = arith.constant " + std::to_string(i + 1) + " : index\n";
      loads += "        %bits" + suffix + " = memref.load %parameters[%index" + suffix +
               "] : " + parameters + "\n";
      loads += "        %scalar" + suffix + " = arith.bitcast %bits" + suffix + " : i32 to f32\n";
    }
    return R"(module attributes {gpu.container_module,
  spirv.target_env = #spirv.target_env<#spirv.vce<v1.0, [Shader, Int64], [SPV_KHR_storage_buffer_storage_class]>, #spirv.resource_limits<>>} {
  gpu.module @kernels {
    gpu.func @)" +
           std::string(indirect ? "kalandra_elementwise_v3"
                       : legacy ? "kalandra_elementwise"
                                : "kalandra_elementwise_v2") +
           "(" + bindings + "%out: memref<?xf32>, %parameters: " + parameters +
           R"()
      kernel attributes {spirv.entry_point_abi = #spirv.entry_point_abi<workgroup_size = [64, 1, 1]>} {
      %block = gpu.block_id x
      %thread = gpu.thread_id x
      %width = gpu.block_dim x
      %base = arith.muli %block, %width : index
      %i = arith.addi %base, %thread : index
      %zero = arith.constant 0 : index
      %count = memref.load %parameters[%zero] : )" +
           parameters + R"(
      %n = arith.index_cast %count : i32 to index
      %valid = arith.cmpi ult, %i, %n : index
      scf.if %valid {
)" + loads +
           code.str() + "        memref.store " + result +
           ", %out[%i] : memref<?xf32>\n      }\n      gpu.return\n    }\n  "
           "}\n}\n";
  }

public:
  explicit Compiler(Module module, ArithmeticMode arithmetic, bool testCpuExports,
                    GpuBackend backend)
      : arithmetic_(arithmetic), backend_(backend), testCpuExports_(testCpuExports),
        syntax(std::move(module)) {}
  std::map<std::string, std::string> run() {
    auto checkedPlacement = kalandra::check({syntax, {}}, {}, {.placement = true});
    if (!checkedPlacement.ok())
      throw Rejected{checkedPlacement.diagnostics.front()};
    placement = std::move(*checkedPlacement.module);
    // Constant shape constructors need no array carrier in a coordinating
    // frame. Preserve their exact literal elements and complete ranked shape.
    for (std::size_t id = 0; id < placement.expressions.size(); ++id) {
      auto &expression = placement.expressions[id];
      auto conversion = std::get_if<ArrayToTensor>(&expression.operation);
      if (!conversion)
        continue;
      const auto *literal =
          std::get_if<TensorLiteral>(&placement.expressions.at(conversion->input).operation);
      if (!literal || !placement.expressions.at(conversion->input).type.array)
        continue;
      bool constant = true, empty = false;
      std::vector<Extent> shape;
      if (conversion->dimensions) {
        for (auto dimension : *conversion->dimensions) {
          auto bits = literalBits(dimension);
          const auto type = node(dimension).type.element;
          if (!bits || !isInteger(type) || *bits > INT64_MAX ||
              (!isUnsigned(type) && (*bits & (UINT64_C(1) << (describeNumeric(type)->bits - 1))))) {
            constant = false;
            break;
          }
          shape.push_back({*bits, 0});
          empty |= *bits == 0;
        }
      } else
        shape.push_back({literal->elements.size(), 0});
      std::uint64_t count = empty ? 0 : 1;
      if (!empty)
        for (const auto &axis : shape) {
          if (*axis.constant > std::uint64_t(INT64_MAX) /
                                   describeNumeric(expression.type.element)->bytes() / count) {
            constant = false;
            break;
          }
          count *= *axis.constant;
        }
      for (auto element : literal->elements)
        constant &= literalBits(element).has_value();
      if (!constant || shape.size() > 8 || count != literal->elements.size())
        continue;
      auto elements = literal->elements;
      expression.operation = TensorLiteral{std::move(elements)};
      expression.type.shape = std::move(shape);
    }
    resumable = continuation::resumableFunctions(placement);
    const auto coordinating = continuation::coordinatingFunctions(placement);
    for (FunctionId id = 0; id < placement.functions.size(); ++id)
      if (!coordinating[id] && !placement.functions[id].resultType.fallible)
        resumable[id] = false;
    Module hostModule = syntax;
    hostModule.functions.clear();
    std::optional<std::size_t> main;
    for (std::size_t i = 0; i < syntax.functions.size(); ++i) {
      const auto &fn = syntax.functions[i];
      auto name = placement.functions.at(i).name;
      if (fn.moduleIndex == 0 && name == "main") {
        main = i;
        continue;
      }
      if (name == "devices" || name == "tensor" || name == "transfer")
        reject(fn.name, "this GPU source slice reserves standard placement/transfer names");
      if (!fn.result)
        reject(fn.span, "mixed source functions require an explicit result");
      if (fn.result->tensor && !fn.result->array)
        tensorType(*fn.result);
      else if (!placement.functions.at(i).resultType.record &&
               placement.functions.at(i).resultType.builtin != BuiltinValue::Function &&
               !resolveNumeric(text(fn.result->name)) && text(fn.result->name) != "Bool")
        reject(fn.result->span,
               "mixed source results require numeric scalars, Bool, numeric/Bool arrays "
               "or rank-zero through rank-eight numeric tensors");
      for (std::size_t parameter = 0; parameter < fn.parameters.size(); ++parameter) {
        const auto &p = fn.parameters[parameter];
        const auto &type = placement.symbols.at(placement.functions.at(i).parameters.at(parameter)).type;
        if (type.record || type.builtin == BuiltinValue::Function)
          continue; // Internal helpers do not imply a host transport ABI.
        if (p.type.tensor && !p.type.array)
          tensorType(p.type);
        else if (!resolveNumeric(text(p.type.name)) && text(p.type.name) != "Bool")
          reject(p.type.span,
                 "mixed source parameters require numeric scalars, Bool, numeric/Bool arrays "
                 "or rank-zero through rank-eight numeric tensors");
      }
      if (coordinating[i])
        continue;
      cpuFunctions.emplace(name, hostModule.functions.size());
      // Synchronous error helpers can be dependencies of ordinary CPU
      // bridges and GPU entries. Only completed results use the bridge ABI.
      cpuExports.push_back(placement.functions.at(i).resultType.fallible ? std::string{} : name);
      hostModule.functions.push_back(fn);
    }
    if (!main)
      reject({}, "missing main");
    // The CPU bridge contains only synchronous helpers. Remove imports of
    // coordinating functions from this projection too; their original resolved
    // identities remain in the placement module and continuation plan.
    using Binding = std::pair<std::size_t, std::string>;
    std::map<Binding, FunctionId> exportedFunctions;
    std::map<Binding, Binding> reexports;
    for (FunctionId i = 0; i < syntax.functions.size(); ++i) {
      const auto &fn = syntax.functions[i];
      if (fn.exported)
        exportedFunctions.emplace(Binding{fn.moduleIndex, text(fn.name)}, i);
    }
    for (const auto &import : syntax.imports)
      if (import.exported && import.target)
        for (const auto &name : import.names)
          reexports.emplace(Binding{import.moduleIndex, text(name.alias.value_or(name.name))},
                            Binding{*import.target, text(name.name)});
    auto projectedAway = [&](Binding binding) {
      // loadModules already rejects cycles and bounds graph depth. Keep this
      // traversal explicitly bounded as the compiler also has in-memory
      // callers.
      for (std::size_t depth = 0; depth <= syntax.imports.size(); ++depth) {
        if (auto found = exportedFunctions.find(binding); found != exportedFunctions.end())
          return found->second == *main || coordinating[found->second];
        auto next = reexports.find(binding);
        if (next == reexports.end())
          return false;
        binding = next->second;
      }
      reject({}, "cyclic module re-export in CPU projection");
    };
    for (auto &import : hostModule.imports)
      std::erase_if(import.names, [&](const ImportedName &name) {
        return import.target && projectedAway({*import.target, text(name.name)});
      });
    std::erase_if(hostModule.imports, [](const Import &import) { return import.names.empty(); });
    auto checked = kalandra::check({std::move(hostModule), {}});
    if (!checked.ok())
      throw Rejected{checked.diagnostics.front()};
    cpu = std::move(*checked.module);
    std::set<FunctionId> fallibleGpuFunctions;
    for (const auto &expression : placement.expressions) {
      const auto *spawn = std::get_if<TaskOperation>(&expression.operation);
      if (!spawn || spawn->prefix != TaskPrefix::Spawn || !spawn->placement ||
          !placement.expressions.at(*spawn->placement).type.device)
        continue;
      const auto *call =
          std::get_if<FunctionCall>(&placement.expressions.at(spawn->operand).operation);
      if (call && placement.functions.at(call->function).resultType.errorRecord)
        fallibleGpuFunctions.insert(call->function);
    }
    for (auto function : fallibleGpuFunctions) {
      auto entry = appendDeviceErrorEntry(placement, function, aggregateEntryWorkRemaining);
      if (!entry)
        reject(placement.functions.at(function).declaration, entry.error());
      deviceErrorEntries.emplace(function, *entry);
    }
    // Checking may append anonymous callback functions after the named source
    // entries. Reserve their indices before appending synthesized host exports.
    cpuExports.resize(cpu.functions.size());
    for (const auto &symbol : cpu.symbols)
      observeExtents(symbol.type);
    for (const auto &expression : cpu.expressions)
      observeExtents(expression.type);
    for (const auto &function : cpu.functions) {
      observeExtents(function.resultType);
      if (function.declaredResult)
        observeExtents(*function.declaredResult);
      for (const auto &requirement : function.preconditions) {
        hostExtent = std::max(hostExtent, requirement.left.identity);
        hostExtent = std::max(hostExtent, requirement.right.identity);
      }
    }
    for (std::size_t i = 0; i < cpu.functions.size(); ++i)
      if (!cpu.functions[i].resultType.fallible)
        declareCpuFunction(i);
    const auto &fn = syntax.functions[*main];
    if (!fn.parameters.empty() || !fn.result || !fn.taskError)
      reject(fn.span, "Metal main requires no parameters, a float32 or tensor "
                      "result, and ! TaskError");
    const auto &mainType = placement.functions.at(*main).resultType;
    if (mainType.record)
      reject(fn.span, "GPU main aggregate output requires a native aggregate CLI result adapter");
    const bool mainArray = mainType.array;
    const bool mainBoolArray = isBoolArray(mainType);
    if (mainArray && mainType.builtin != BuiltinValue::Numeric && !mainBoolArray)
      reject(fn.span, "GPU main array results require numeric or Bool elements");
    bool scalarResult = !mainArray && !fn.result->tensor;
    const bool booleanResult = scalarResult && text(fn.result->name) == "Bool";
    if (scalarResult && !booleanResult && !resolveNumeric(text(fn.result->name)))
      reject(fn.span, "Metal main requires float32 or a rank-one through "
                      "rank-eight float32 tensor");
    if (!scalarResult && !mainArray)
      tensorType(*fn.result);
    const auto mainElement =
        booleanResult || mainBoolArray ? NumericType::UInt8 : mainType.element;
    const auto mainWidth = describeNumeric(mainElement)->bytes();
    auto planned = continuation::plan(
        placement,
        {.preserveCalls = true, .entry = *main, .valueScopes = true, .deviceValues = true});
    if (!planned)
      reject(fn.span, planned.error());
    continuationCpu = &planned->kernels;
    continuationKernels = planned->kernelIds;
    continuationImports = planned->importedFunctions;
    for (const auto &[sourceId, targetId] : continuationKernels) {
      (void)sourceId;
      declareCpuFunction(targetId, continuationCpu);
    }
    auto result = lowerPlan(*planned->plans[*main], *main, true);
    (void)result;
    // Compile each reachable resumable child once. Keep the main frame's state
    // separate while numeric exports and shader entries remain module-wide.
    auto mainFields = std::move(fields), mainSteps = std::move(steps),
         mainGroups = std::move(plannedGroups);
    auto mainPC = std::move(plannedPC);
    auto mainSites = std::move(gpuAwaitSites);
    auto mainParameters = std::move(plannedParameters);
    const auto mainNext = next;
    auto mainArrays = std::move(arrayFields);
    std::string helperDeclarations, helperBodies;
    for (FunctionId id = 0; id < planned->plans.size(); ++id) {
      if (id == *main || planned->plans[id]->code.empty())
        continue;
      fields.clear();
      arrayFields.clear();
      steps.clear();
      plannedGroups.clear();
      plannedPC.clear();
      gpuAwaitSites.clear();
      plannedParameters.clear();
      next = 0;
      activeGroup.clear();
      admissionExit.reset();
      lowerPlan(*planned->plans[id], id);
      helperDeclarations += helperPacket(id);
      helperBodies += helperBody(id);
    }
    fields = std::move(mainFields);
    steps = std::move(mainSteps);
    plannedGroups = std::move(mainGroups);
    plannedPC = std::move(mainPC);
    gpuAwaitSites = std::move(mainSites);
    plannedParameters = std::move(mainParameters);
    next = mainNext;
    arrayFields = std::move(mainArrays);
    if (entries.empty())
      reject(fn.span, "no GPU placement in source");
    std::map<std::string, std::string> outputs;
    outputs["placement.ir"] = dumpTypedIR(placement);
    outputs["continuation.plan"] = continuation::dump(*planned);
    for (auto [function, index] : entries) {
      auto base = "kernel" + std::to_string(index);
      if (auto found = numericPrograms.find(function); found != numericPrograms.end()) {
        for (std::size_t stage = 0; stage < found->second.kernels.size(); ++stage) {
          const auto &shader = found->second.kernels[stage];
          auto name = base + (stage ? "-stage" + std::to_string(stage) : "");
          if (backend_ != GpuBackend::Metal && shader.element == NumericType::Float32 &&
              std::any_of(found->second.code.begin(), found->second.code.end(),
                          [&](const auto &op) {
                            return op.kernel == stage && op.kind == NumericProgramKind::Matmul;
                          }))
            outputs[name + (backend_ == GpuBackend::Cuda ? ".cuda-matmul" : ".vulkan-matmul")] =
                backend_ == GpuBackend::Cuda ? "kalandra-cuda-matmul-v1\ntype=float32\n"
                                             : "kalandra-vulkan-matmul-v1\ntype=float32\n";
          if (backend_ != GpuBackend::Metal && shader.transform) {
            outputs[name +
                    (backend_ == GpuBackend::Cuda ? ".cuda-transform" : ".vulkan-transform")] =
                std::string(backend_ == GpuBackend::Cuda ? "kalandra-cuda-transform-v1\nkind="
                                                         : "kalandra-vulkan-transform-v1\nkind=") +
                std::string(tensorTransformName(*shader.transform)) +
                "\ntype=" + std::string(describeNumeric(shader.element)->name) + "\n";
            continue;
          }
          outputs[name + (shader.nativeMetal.empty() ? ".mlir" : ".native.metal")] =
              shader.nativeMetal.empty() ? shader.mlir : shader.nativeMetal;
          if (backend_ == GpuBackend::Vulkan && shader.checkpointedTensorArithmetic)
            outputs[name + ".vulkan-outline-i64-sub"] = "";
        }
        continue;
      }
      if (auto found = numericShaders.find(function); found != numericShaders.end()) {
        outputs[base + ".mlir"] = found->second.mlir;
        if (backend_ == GpuBackend::Vulkan && found->second.checkpointedTensorArithmetic)
          outputs[base + ".vulkan-outline-i64-sub"] = "";
        if (!found->second.reduction.empty())
          outputs[base + "-reduce.mlir"] = found->second.reduction;
        continue;
      }
      outputs[base + ".mlir"] = shader(function);
      if (auto entry = primitiveEntry(function); entry) {
        if ((entry->kind == PrimitiveEntry::Sum && entry->mapped) ||
            entry->kind == PrimitiveEntry::MatrixSum)
          outputs[base + "-reduce.mlir"] = reductionShader();
        if (entry->leftMapped)
          outputs[base + "-left.mlir"] = shader(function, nullptr, false, 0);
        if (entry->rightMapped)
          outputs[base + "-right.mlir"] = shader(function, nullptr, false, 1);
        if (entry->post) {
          std::vector<std::uint32_t> reads;
          auto post = shader(function, &reads, true);
          if ((entry->kind == PrimitiveEntry::Matrix || entry->kind == PrimitiveEntry::MatrixSum) &&
              !reads.empty() && reads.front() == 0 && reads.size() <= 27 &&
              !retainedChoiceBindings(function, reads.size())) {
            unsigned scalars = 0;
            for (auto parameter : cpu.functions[function].parameters)
              scalars += !cpu.symbols[parameter].type.tensor;
            post = "// kalandra.matmul.epilogue " + std::to_string(reads.size()) + " " +
                   std::to_string(scalars) + "\n" + post;
          }
          outputs[base + "-post.mlir"] = std::move(post);
        }
        if (entry->scalarPost)
          outputs[base + "-scalar.mlir"] = shader(function, nullptr, true, -1, true);
      }
      if (const auto &graph = matrixGraph(function); graph && graph->reduce) {
        outputs[base + "-reduce.mlir"] = reductionShader();
        if (!graph->tensorResult && graph->reductions.size() >= 2)
          outputs[base + "-join.mlir"] = shader(function, nullptr, true, -1, true);
        else if (!graph->tensorResult && graph->scalarPost)
          outputs[base + "-scalar.mlir"] = shader(function, nullptr, true, -1, true);
      }
      if (const auto &graph = matrixGraph(function); graph)
        for (std::size_t stage = 0; stage < graph->maps.size(); ++stage)
          if (graph->maps[stage]) {
            std::vector<std::uint32_t> reads;
            auto map = shader(function, &reads, false, -1, false, stage);
            // Optional same-cell unary epilogue. The runtime also requires a
            // sole consumer before eliding the original matrix value.
            if (stage && !graph->maps[stage - 1] && reads.size() == 1 &&
                reads.front() == graph->inputs + stage - 1) {
              unsigned scalars = 0;
              for (auto parameter : cpu.functions[function].parameters)
                scalars += !cpu.symbols[parameter].type.tensor;
              map = "// kalandra.matmul.epilogue 1 " + std::to_string(scalars) + "\n" + map;
            }
            outputs[base + "-map" + std::to_string(stage) + ".mlir"] = std::move(map);
          }
      if (matrixFunction(function) ||
          (matrixGraph(function) && matrixGraph(function)->matrixCount)) {
        outputs[base + "-rows2.mlir"] = matmulRows2Shader;
        outputs[base + "-quad.mlir"] = matmulQuadShader;
      }
    }
    mlir::DialectRegistry registry;
    mlir::registerAllDialects(registry);
    mlir::MLIRContext context(registry);
    for (auto &[name, ir] : outputs) {
      if (!name.ends_with(".mlir"))
        continue;
      auto parsed = mlir::parseSourceString<mlir::ModuleOp>(ir, &context);
      if (!parsed || mlir::failed(mlir::verify(*parsed)))
        reject(fn.span, "generated GPU IR failed verification");
    }
    lowerCpuModules(outputs, fn.span);
    std::ostringstream host;
    host << (backend_ != GpuBackend::Metal ? "#include \"Language.h\"\n" : "#include \"Metal.h\"\n")
         << R"(#include "../runtime/HostArrays.h"
#include "../runtime/DeviceArrayInputJob.h"
#include "../runtime/DeviceGraphCapture.h"
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
using namespace kalandra::runtime;
)"
         << (backend_ != GpuBackend::Metal
                 ? (backend_ == GpuBackend::Cuda
                        ? "namespace m = kalandra::runtime::cuda::language;\n"
                        : "namespace m = kalandra::runtime::vulkan::language;\n")
                 : "namespace m = kalandra::runtime::metal;\n")
         << " " << aggregateDeclarations << declarations << helperDeclarations << cpuEntries;
    if (mainArray)
      host << "const ValueLayout &mainArrayLayout=hostArrayLayout<" << elementName(mainElement)
           << "," << (mainBoolArray ? "true" : "false") << ">();\n";
    if (scalarResult)
      host << (booleanResult ? "const ValueLayout &scalarLayout=boolScalarLayout();\n"
                             : "const ValueLayout &scalarLayout=*numericScalarLayout(" +
                                   elementName(mainElement) + ");\n");
    const std::string mainSuccessLayout =
        mainArray ? "mainArrayLayout" : scalarResult ? "scalarLayout" : "m::Runtime::bufferLayout()";
    if (result.applicationError) {
      const auto family = applicationError(result).record.value();
      const auto valueType = mainArray ? tensorDescriptor(mainElement, 1)
          : scalarResult ? booleanResult ? std::string("bool") : scalarType(mainElement)
                         : std::string("m::Buffer");
      host << outcomeDeclaration("MainOutcome", "mainOutcomeLayout", valueType, family,
                                 mainSuccessLayout, 0x4b414c4750554552ull)
           <<              "int reportMainError(const " << recordName(family) << " &error){\n"
              "std::int64_t tag=0;std::memcpy(&tag,error.data.data(),sizeof(tag));\nswitch(tag){\n";
      const auto &record = placement.records.at(family);
      for (std::size_t v = 0; v < record.variants.size(); ++v) {
        const auto [origin, variant] = errorOrigin(placement, family, v);
        const auto &declared = placement.records.at(origin);
        host << "case " << v + 1 << ":";
        if (declared.taskErrorFamily) {
          const auto &field = aggregateTypes.at(family).fields.at(record.variants[v].fields.front());
          host << "{Failure failure{};static_assert(sizeof(failure)==" << field.nativeBytes
               << ");std::memcpy(&failure,error.data.data()+" << field.nativeOffset
               << ",sizeof(failure));if(static_cast<unsigned>(failure.kind)!=" << variant + 1
               << ")return 1;std::fprintf(stderr,\"Metal task failed: %d/%lld\\n\","
                  "int(failure.kind),(long long)failure.code);return 1;}\n";
        } else
          host << "std::fprintf(stderr,\"%s.%s\\n\",\"" << declared.name
               << "\",\"" << declared.variants.at(variant).name << "\");return 89;\n";
      }
      host << "default:std::fputs(\"invalid GPU application error result\\n\",stderr);return 1;}}\n";
    }
    host << arrayScopeDeclaration();
    host << R"(struct PacketData { m::Runtime *runtime; const char *directory; };
struct Frame { m::Runtime *runtime; std::string directory; unsigned pc=1;
)";
    for (auto &f : fields)
      host << "  " << f << '\n';
    if (!arrayFields.empty())
      host << "HostArrayScopes arrayScopes{};\n";
    host << "};\nstruct GpuAwaitSite { unsigned pc; std::size_t group; };\n";
    host << "[[maybe_unused]] constexpr std::array<GpuAwaitSite," << gpuAwaitSites.size()
         << "> gpuAwaitSites{{";
    for (auto site : gpuAwaitSites)
      host << "{" << plannedPC.at(site.instruction) << "," << site.group << "},";
    host << "}};\ninline GroupHandle continuationGroup(const Frame "
            "&f,std::size_t id){switch(id){\n";
    for (std::size_t i = 0; i < plannedGroups.size(); ++i)
      host << "case " << i << ":return " << plannedGroups[i] << ";\n";
    host << "default:return {};}}\n";
    host << "bool construct([[maybe_unused]] Invocation &context, void *out, const Packet &p) {\n"
            "auto &data=*static_cast<const PacketData *>(p.data);\n"
            "[[maybe_unused]] auto &f=*new(out) Frame{data.runtime,data.directory};\n";
    if (!arrayFields.empty())
      host << "if(!f.arrayScopes.initialize(context,arrayLayouts)){f.~Frame();return false;}\n";
    host << "return true;}\n";
    host << R"(void destroy(void *p) noexcept { static_cast<Frame *>(p)->~Frame(); }
bool readBuffer(Task &task, TaskHandle child, m::Buffer &output) {
  auto failure=task.failure(child);
  if (failure.kind!=TaskFailure::None) { task.fault(failure.code ? failure.code : std::int64_t(m::Error::Device)); return false; }
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  if (!task.read(child,m::Runtime::bufferLayout(),slot)) return false;
  auto *value=std::launder(reinterpret_cast<m::Buffer *>(slot)); output=*value; value->~Buffer(); return true;
}
template<kalandra::NumericType Element>
bool readScalar(Task &task,TaskHandle child,NumericValue<Element> &output) {
  auto failure=task.failure(child);
  if(failure.kind!=TaskFailure::None) {task.fault(failure.code ? failure.code : std::int64_t(m::Error::Device)); return false;}
  return task.read(child,*numericScalarLayout(Element),&output);
}
bool readBool(Task &task,TaskHandle child,bool &output) {
  auto failure=task.failure(child);
  if(failure.kind!=TaskFailure::None){task.fault(failure.code ? failure.code : std::int64_t(m::Error::Device));return false;}
  return task.read(child,boolScalarLayout(),&output);
}
void resume(Task &task) {
  auto &f=*static_cast<Frame *>(task.frame()); [[maybe_unused]] auto &r=*f.runtime;
  for (;;) { switch(f.pc) {
)";
    for (std::size_t i = 0; i < steps.size(); ++i)
      host
          << "case " << i + 1 << ": {\n"
          << (arrayFields.empty()
                  ? ""
                  : "HostArrayWork work(f.arrayScopes);if(!work){hostArrayFailure(task);return;}\n")
          << steps[i] << "f.pc=" << i + 2 << ";\n} [[fallthrough]];\n";
    host << "default: task.fault(3); return;\n} }\n}\n";
    host
        << R"(const TaskEntry entry{1,0x4b414c47505501,sizeof(PacketData),alignof(PacketData),sizeof(Frame),alignof(Frame),construct,destroy,resume,)"
        << (result.applicationError ? "&mainOutcomeLayout" : "&" + mainSuccessLayout) << "};\n"
        << helperBodies << R"(int main(int argc,char **argv) {
  if (argc!=1 && argc!=2) { std::fprintf(stderr,"expected optional shader directory\n"); return 2; }
  const auto directory=argc==2 ? std::string(argv[1]) : std::filesystem::absolute(argv[0]).parent_path().string();
  auto made=m::Runtime::create();
  if (!made) { std::fprintf(stderr,"Metal unavailable: %lld\n",(long long)made.error()); return 1; }
  auto runtime=std::move(*made);
  TaskLimits limits;
  if (auto *configured=std::getenv("KALANDRA_WORKERS")) {
    unsigned count=0; auto end=configured+std::strlen(configured); auto parsed=std::from_chars(configured,end,count);
    if (parsed.ec!=std::errc{} || parsed.ptr!=end || count<1 || count>64) return 2;
    limits.workers=count;
  }
  auto runner=ClosedTaskRunner::create(limits); if (!runner) return 1;
  PacketData packet{runtime.get(),directory.c_str()};
  Invocation context;
)";
    if (result.applicationError)
      host << "alignas(MainOutcome) std::byte slot[sizeof(MainOutcome)];\n"
              "auto failure=runner->run(entry,{entry.packetLayout,&packet,sizeof(packet)},"
              "mainOutcomeLayout,context,slot);\n";
    else if (mainArray)
      host << tensorDescriptor(mainElement, 1) << " value{};\n"
              "auto failure=runner->run(entry,{entry.packetLayout,&packet,sizeof(packet)},"
              "mainArrayLayout,context,&value);\n";
    else if (scalarResult)
      host << (booleanResult ? "bool" : scalarType(mainElement))
           << " value{};\n"
              "auto "
              "failure=runner->run(entry,{entry.packetLayout,&packet,sizeof(packet)},scalarLayout,"
              "context,&value);\n";
    else
      host << "alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];\n"
              "auto "
              "failure=runner->run(entry,{entry.packetLayout,&packet,sizeof(packet)},m::Runtime::"
              "bufferLayout(),context,slot);\n";
    host << R"(
  int status=0;
  if (failure.kind!=TaskFailure::None) { std::fprintf(stderr,"Metal task failed: %d/%lld\n",int(failure.kind),(long long)failure.code); status=1; }
  else {
)";
    if (result.applicationError)
      host << "auto &outcome=*std::launder(reinterpret_cast<MainOutcome *>(slot));\n"
              "if(outcome.failed)status=reportMainError(outcome.error);else{\n"
           << (scalarResult || mainArray ? "auto &value=outcome.value;\n"
                                         : "auto *value=&outcome.value;\n");
    if (mainArray) {
      host << "{auto retained=retainCpuTensor(context,value.view());if(!retained)status=1;else{\n"
              "std::printf(\"[\");for(std::uint64_t i=0;i<std::uint64_t(value.sizes[0]);++i){\n"
              "std::uint64_t bits=0;std::memcpy(&bits,retained->data().data()+i*"
           << mainWidth << "," << mainWidth << ");\n";
      if (mainBoolArray)
        host << "if(i)std::printf(\",\");std::printf(\"%s\",bits ? \"true\" : \"false\");\n";
      else
        host << "std::printf(i ? \",%0*llx\" : \"%0*llx\"," << mainWidth * 2
             << ",static_cast<unsigned long long>(bits));\n";
      host << "}std::puts(\"]\");}}context.settle();if(context.status()||context.live())status=1;\n";
    } else if (booleanResult) {
      host << "std::puts(value ? \"true\" : \"false\");\n";
    } else if (scalarResult) {
      host << "std::printf(\"%0*llx\\n\"," << mainWidth * 2
           << ",static_cast<unsigned long long>(std::bit_cast<std::uint" << mainWidth * 8
           << "_t>(value)));\n";
    } else {
      if (!result.applicationError)
        host << "auto *value=std::launder(reinterpret_cast<m::Buffer *>(slot));\n";
      host << R"(    auto cpu=runtime->toCpuTyped<)"
           << elementName(mainElement) << "," << fn.result->dimensions.size()
           << R"(>(context,*value);
    if (!cpu) status=1;
    else { std::printf("["); for (std::uint64_t i=0;i<value->bytes()/)"
           << mainWidth << R"(;++i) {
      std::uint64_t bits=0; std::memcpy(&bits,cpu->aligned+i,)"
           << mainWidth << R"(); std::printf(i ? ",%0*llx" : "%0*llx",)" << mainWidth * 2
           << R"(,static_cast<unsigned long long>(bits));
    } std::puts("]"); context.release(cpu->allocated); }
)";
      if (!result.applicationError)
        host << "value->~Buffer();\n";
    }
    if (result.applicationError)
      host << "}\nmainOutcomeLayout.destroy(&outcome);context.settle();"
              "if(context.status()||context.live())status=1;\n";
    host << R"(
  }
  auto s=runtime->stats(); auto t=runner->stats();
  if (s.bytes || s.metalBuffers || s.hostBackings || s.deviceBackings || s.inFlight || s.stagingBuffers || s.argumentBytes || s.argumentPacks || t.records || t.groups || t.externalPending || t.retainedBytes) return 1;
  return status;
}
)";
    auto hostText = host.str();
    if (backend_ != GpuBackend::Metal) {
      hostText = "#include \"ExecutablePath.h\"\n" + hostText;
      const std::string oldDirectory = "const auto directory=argc==2 ? std::string(argv[1]) : "
                                       "std::filesystem::absolute(argv[0]).parent_path().string();";
      const auto directoryAt = hostText.find(oldDirectory);
      if (directoryAt == std::string::npos)
        reject({}, "missing Vulkan artifact-directory initialization");
      hostText.replace(directoryAt, oldDirectory.size(),
                       "auto resolvedDirectory=argc==2 ? "
                       "std::expected<std::string,std::error_code>(std::string(argv[1])) : "
                       "kalandra::runtime::vulkan::executableDirectoryUtf8();\n"
                       "  if (!resolvedDirectory) { std::fprintf(stderr,\"Vulkan artifact "
                       "directory unavailable\\n\"); return 1; }\n"
                       "  const auto &directory=*resolvedDirectory;");
      if (arithmetic_ == ArithmeticMode::HardwareWithSoftwareFallback) {
        const std::string create = "auto made=m::Runtime::create();";
        const auto at = hostText.find(create);
        if (at == std::string::npos)
          reject({}, "missing Vulkan arithmetic-policy initialization");
        hostText.replace(at, create.size(),
                         "auto made=m::Runtime::create({.allowSoftwareFallback=true});");
      }
      const std::string destruction = "value->~Buffer()";
      for (std::size_t at = 0; (at = hostText.find(destruction, at)) != std::string::npos;)
        hostText.replace(at, destruction.size(), "std::destroy_at(value)");
      for (std::size_t at = 0; (at = hostText.find("Metal unavailable", at)) != std::string::npos;)
        hostText.replace(at, 17,
                         backend_ == GpuBackend::Cuda ? "CUDA unavailable" : "Vulkan unavailable");
      for (std::size_t at = 0; (at = hostText.find("Metal task failed", at)) != std::string::npos;)
        hostText.replace(at, 17,
                         backend_ == GpuBackend::Cuda ? "CUDA task failed" : "Vulkan task failed");
      const std::string metalCleanup =
          "s.bytes || s.metalBuffers || s.hostBackings || s.deviceBackings || s.inFlight || "
          "s.stagingBuffers || s.argumentBytes || s.argumentPacks";
      if (auto at = hostText.find(metalCleanup); at != std::string::npos)
        hostText.replace(at, metalCleanup.size(), "s.bytes || s.inFlight || s.argumentBytes");
    }
    if (backend_ == GpuBackend::Cuda) {
      const std::string old = "kalandra::runtime::vulkan::executableDirectoryUtf8()";
      if (auto at = hostText.find(old); at != std::string::npos)
        hostText.replace(at, old.size(), "kalandra::runtime::cuda::executableDirectoryUtf8()");
    }
    outputs["host.cpp"] = std::move(hostText);
    std::string manifest = "kernel_count=" + std::to_string(entries.size()) + "\n";
    for (auto [function, index] : entries) {
      if (auto found = numericPrograms.find(function); found != numericPrograms.end()) {
        const auto prefix = "kernel" + std::to_string(index) + ".";
        manifest += prefix +
                    "numeric_program_instructions=" + std::to_string(found->second.code.size()) +
                    "\n" + prefix +
                    "numeric_program_stages=" + std::to_string(found->second.kernels.size()) +
                    "\n" + prefix + "numeric_type=" +
                    std::string(describeNumeric(found->second.type.builtin == BuiltinValue::Bool
                                                    ? NumericType::UInt8
                                                    : found->second.type.element)
                                    ->name) +
                    "\n";
        continue;
      }
      if (auto found = numericShaders.find(function); found != numericShaders.end()) {
        const auto prefix = "kernel" + std::to_string(index) + ".";
        std::size_t tensors = 0, scalars = 0;
        for (auto parameter : cpu.functions.at(function).parameters)
          (cpu.symbols.at(parameter).type.tensor ? tensors : scalars)++;
        std::string reads;
        for (auto read : found->second.reads)
          reads += (reads.empty() ? "" : ",") + std::to_string(read);
        manifest += prefix +
                    "numeric_type=" + std::string(describeNumeric(found->second.element)->name) +
                    "\n" + prefix +
                    (found->second.indirect ? "binding_layout=argument-buffer-tier2\n"
                                            : "binding_layout=direct\n") +
                    prefix + "tensors=" + std::to_string(tensors) + "\n" + prefix +
                    "scalars=" + std::to_string(scalars) + "\n" + prefix +
                    "tensor_bindings=" + reads + "\n";
        continue;
      }
      if (const auto &graph = matrixGraph(function); graph) {
        const auto prefix = "kernel" + std::to_string(index) + ".";
        std::string edges;
        for (auto [a, b] : graph->products)
          edges += (edges.empty() ? "" : ",") + std::to_string(a) + ":" + std::to_string(b);
        manifest += prefix + "matrix_graph_inputs=" + std::to_string(graph->inputs) + "\n" +
                    prefix + "matrix_graph_products=" + std::to_string(graph->matrixCount) + "\n" +
                    prefix + "matrix_graph_stages=" + std::to_string(graph->products.size()) +
                    "\n" + prefix + "matrix_graph_edges=" + edges + "\n";
        for (const auto &[stage, operands] : graph->mapInputs) {
          std::string bindings;
          for (auto input : operands)
            bindings += (bindings.empty() ? "" : ",") + std::to_string(input);
          manifest += prefix + "map" + std::to_string(stage) + "_inputs=" + bindings + "\n";
        }
        if (!graph->broadcasts.empty()) {
          std::string bindings;
          for (auto [stage, reduction] : graph->broadcasts)
            bindings += (bindings.empty() ? "" : ",") + std::to_string(stage) + ":" +
                        std::to_string(reduction);
          manifest += prefix + "reduction_broadcasts=" + bindings + "\n";
        }
        for (std::size_t stage = 0; stage < graph->maps.size(); ++stage)
          if (graph->maps[stage]) {
            std::vector<std::uint32_t> reads;
            shader(function, &reads, false, -1, false, stage);
            manifest += prefix + "map" + std::to_string(stage) + "_stage=kernel" +
                        std::to_string(index) + "-map" + std::to_string(stage) + "\n";
            manifest += prefix + "map" + std::to_string(stage) +
                        "_tensors=" + std::to_string(reads.size()) + "\n";
          }
        if (graph->reduce)
          manifest += prefix + "reduction_stage=kernel" + std::to_string(index) + "-reduce\n";
        if (graph->reductions.size() >= 2 || graph->tensorResult) {
          std::string roots;
          for (auto root : graph->reductions)
            roots += (roots.empty() ? "" : ",") + std::to_string(root);
          manifest += prefix + "reduction_inputs=" + roots + "\n";
          if (graph->tensorResult)
            manifest += prefix + "graph_result=tensor\n";
          else
            manifest += prefix + "join_stage=kernel" + std::to_string(index) + "-join\n";
        }
        if (!graph->tensorResult && graph->scalarPost && graph->reductions.size() < 2) {
          std::size_t scalars = 0;
          for (auto parameter : cpu.functions[function].parameters)
            scalars += !cpu.symbols[parameter].type.tensor;
          manifest += prefix + "scalar_stage=kernel" + std::to_string(index) + "-scalar\n" +
                      prefix + "scalar_parameters=" + std::to_string(scalars) + "\n";
        }
        continue;
      }
      if (auto entry = primitiveEntry(function); entry && matrixFunction(function)) {
        unsigned tensors = 0, scalars = 0;
        for (auto parameter : cpu.functions[function].parameters) {
          tensors += cpu.symbols[parameter].type.tensor;
          scalars += !cpu.symbols[parameter].type.tensor;
        }
        for (int operand = 0; operand < 2; ++operand) {
          if (!(operand == 0 ? entry->leftMapped : entry->rightMapped))
            continue;
          const auto label = operand == 0 ? "left" : "right";
          const auto prefix = "kernel" + std::to_string(index) + "." + label + "_";
          std::vector<std::uint32_t> reads;
          shader(function, &reads, false, operand);
          std::string bindings;
          for (auto input : reads)
            bindings += (bindings.empty() ? "" : ",") + std::to_string(input);
          manifest +=
              prefix + "stage=kernel" + std::to_string(index) + "-" + label + "\n" + prefix +
              "tensors=" + std::to_string(tensors) + "\n" + prefix +
              "scalars=" + std::to_string(scalars) + "\n" + prefix + "tensor_bindings=" + bindings +
              "\n" + prefix + "binding_layout=" +
              (!retainedChoiceBindings(function, reads.size()) &&
                       kalandra::runtime::metal::directElementwiseLayout(reads.size(), scalars)
                   ? "direct"
                   : "argument_buffer_tier2") +
              "\n";
        }
      }
      if (auto entry = primitiveEntry(function); entry && entry->post) {
        std::size_t scalars = 0, tensors = 0;
        for (auto parameter : cpu.functions[function].parameters) {
          scalars += !cpu.symbols[parameter].type.tensor;
          tensors += cpu.symbols[parameter].type.tensor;
        }
        manifest += "kernel" + std::to_string(index) + ".epilogue_stage=kernel" +
                    std::to_string(index) + "-post\n" + "kernel" + std::to_string(index) +
                    ".epilogue_scalars=" + std::to_string(scalars) + "\n";
        if (matrixFunction(function)) {
          std::vector<std::uint32_t> reads;
          shader(function, &reads, true);
          std::string bindings;
          for (auto input : reads)
            bindings += (bindings.empty() ? "" : ",") + std::to_string(input);
          const auto prefix = "kernel" + std::to_string(index) + ".epilogue_";
          manifest +=
              prefix + "tensors=" + std::to_string(tensors + 1) + "\n" + prefix +
              "tensor_bindings=" + bindings + "\n" + prefix + "binding_layout=" +
              (!retainedChoiceBindings(function, reads.size()) &&
                       kalandra::runtime::metal::directElementwiseLayout(reads.size(), scalars)
                   ? "direct"
                   : "argument_buffer_tier2") +
              "\n";
        }
      }
      if (auto entry = primitiveEntry(function); entry && entry->scalarPost) {
        std::size_t scalars = 0;
        for (auto parameter : cpu.functions[function].parameters)
          scalars += !cpu.symbols[parameter].type.tensor;
        manifest += "kernel" + std::to_string(index) + ".scalar_stage=kernel" +
                    std::to_string(index) + "-scalar\n" + "kernel" + std::to_string(index) +
                    ".scalar_parameters=" + std::to_string(scalars) + "\n";
      }
      if (auto entry = primitiveEntry(function); entry && entry->kind == PrimitiveEntry::MatrixSum)
        manifest += "kernel" + std::to_string(index) + ".reduction_stage=kernel" +
                    std::to_string(index) + "-reduce\n";
      if ((reduction(function) && !primitiveEntry(function)->mapped) || matrixFunction(function))
        continue;
      unsigned tensors = 0, scalars = 0;
      std::string order;
      for (auto parameter : cpu.functions[function].parameters) {
        const auto &type = cpu.symbols[parameter].type;
        if (!order.empty())
          order += ",";
        if (type.tensor) {
          ++tensors;
          order += "tensor" + std::to_string(type.shape.size());
        } else {
          ++scalars;
          order += "float32";
        }
      }
      auto prefix = "kernel" + std::to_string(index) + ".";
      std::vector<std::uint32_t> reads;
      shader(function, &reads);
      std::string bindings;
      for (auto input : reads)
        bindings += (bindings.empty() ? "" : ",") + std::to_string(input);
      if (auto entry = primitiveEntry(function); entry && entry->mapped)
        manifest += prefix + "reduction_stage=kernel" + std::to_string(index) + "-reduce\n";
      manifest += prefix + "binding_layout=" +
                  (!retainedChoiceBindings(function, reads.size()) &&
                           kalandra::runtime::metal::directElementwiseLayout(reads.size(), scalars)
                       ? "direct"
                       : "argument_buffer_tier2") +
                  "\n";
      manifest += prefix + "tensor_bindings=" + bindings + "\n";
      manifest += prefix + "tensors=" + std::to_string(tensors) + "\n" + prefix +
                  "scalars=" + std::to_string(scalars) + "\n" + prefix + "source_order=" + order +
                  "\n";
    }
    manifest += "arithmetic=" + std::string(arithmeticModeName(arithmetic_)) + "\n";
    if (backend_ == GpuBackend::Vulkan)
      manifest += "arithmetic_policy=" + std::string(arithmeticModeName(arithmetic_)) + "\n";
    outputs["manifest.txt"] = std::move(manifest);
    return outputs;
  }
};
} // namespace
std::expected<std::map<std::string, std::string>, Diagnostic>
kalandra::emitGpuSupervisor(Module module, ArithmeticMode arithmetic, bool testCpuExports,
                            GpuBackend backend) {
  try {
    return Compiler(std::move(module), arithmetic, testCpuExports, backend).run();
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}
