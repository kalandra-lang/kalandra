#include "../metal/ElementwiseLayout.h"
#include "Build.h"
#include "MatmulShader.h"
#include "MatmulTiles.h"
#include "NumericProgram.h"
#include "NumericShader.h"
#include "../third_party/mlx-steel/SteelUpstream.h"
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
#include "llvm/IR/Module.h"
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
  enum Kind { Tensor, Target, Task, Scalar, Boolean, Error, FaultKind, Text } kind;
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
};
class Compiler {
  ArithmeticMode arithmetic_;
  bool testCpuExports_;
  Module syntax;
  std::map<std::string, std::size_t> cpuFunctions;
  TypedModule cpu;
  TypedModule placement;
  std::vector<std::string> cpuExports;
  std::set<FunctionId> usedCpuFunctions;
  std::string declarations, cpuEntries;
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
  void admitGpuTypes(FunctionId root) const {
    std::set<FunctionId> visited;
    std::vector<FunctionId> pending{root};
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
      };
      supported(fn.resultType);
      for (auto parameter : fn.parameters)
        supported(cpu.symbols[parameter].type);
      for (auto value : fn.expressions) {
        const auto &expression = cpu.expressions[value];
        const auto &op = expression.operation;
        if (auto *call = std::get_if<FunctionCall>(&op))
          pending.push_back(call->function);
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
        if (requiresDouble)
          reject(expression.span, "Metal has no hardware float64 arithmetic; explicitly select "
                                  "--arithmetic hardware-with-software-fallback or software");
      }
    }
  }

  std::map<std::size_t, std::size_t> entries;
  std::map<FunctionId, NumericShader> numericShaders;
  std::map<FunctionId, NumericProgram> numericPrograms;
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
  static std::string valueStorage(const Value &value) {
    return value.kind == Value::Tensor    ? "m::Buffer"
           : value.kind == Value::Boolean ? "bool"
                                          : scalarType(value.element);
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
  void declareCpuFunction(FunctionId id) {
    const auto &fn = cpu.functions.at(id);
    declarations +=
        "extern \"C\" " +
        (fn.resultType.tensor                          ? std::string("void")
         : fn.resultType.builtin == BuiltinValue::Bool ? std::string("bool")
                                                       : scalarType(fn.resultType.element)) +
        " _mlir_ciface_kalandra_cpu_" + std::to_string(id) + "(";
    if (fn.resultType.tensor)
      declarations += tensorDescriptor(fn.resultType.element, fn.resultType.shape.size()) + "*,";
    for (auto parameter : fn.parameters) {
      const auto &type = cpu.symbols.at(parameter).type;
      declarations += type.tensor ? tensorDescriptor(type.element, type.shape.size()) + "*,"
                      : type.builtin == BuiltinValue::Bool ? "bool,"
                                                           : scalarType(type.element) + ",";
    }
    declarations += "Invocation*);\n";
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
                bool inlineOperation = false, bool spawned = false) {
    usedCpuFunctions.insert(function);
    const auto &fn = cpu.functions[function];
    if (inputs.size() != fn.parameters.size())
      reject(span, "CPU function argument count mismatch");
    const bool tensorResult = fn.resultType.tensor;
    const bool booleanResult = fn.resultType.builtin == BuiltinValue::Bool;
    if (spawned && booleanResult)
      reject(span, "Boolean CPU task results are not implemented by the Metal bridge");
    auto out = spawned ? std::string("value")
                       : field(tensorResult    ? "m::Buffer"
                               : booleanResult ? "bool"
                                               : scalarType(fn.resultType.element));
    Value value{tensorResult    ? Value::Tensor
                : booleanResult ? Value::Boolean
                                : Value::Scalar,
                spawned ? 0 : next - 1};
    value.element = fn.resultType.element;
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
      const auto &type = cpu.symbols[fn.parameters[i]].type;
      if (type.tensor) {
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
          reject(span, "CPU argument requires its declared float32 or Bool type");
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
    if (tensorResult)
      step += tensorDescriptor(value.element, value.rank) + " result{};\n";
    step += (tensorResult ? "" : "f." + out + " = ") + std::string("_mlir_ciface_kalandra_cpu_") +
            std::to_string(function) + "(" + (tensorResult ? "&result," : "") + arguments +
            "&task.context());\n";
    auto [line, column] = syntax.source->location(span.begin);
    const auto location = inlineOperation ? std::to_string(line) + "," + std::to_string(column)
                                          : "task.context().line(),task.context().column()";
    step += "if (task.context().status()) { task.fault(task.context().status(), " + location +
            "); return; }\n";
    if (spawned)
      step += "if(task.cancelled()) return;\n";
    if (tensorResult) {
      // Acquire the immutable backing lease before releasing the returned
      // memref owner. The buffer-deallocation pipeline gives C-interface tensor
      // results owned storage.
      step += "auto imported = r.fromCpu(task.context(), result.view());\n";
      step += "task.context().release(result.allocated);\n";
      step += "if (!imported) { " + error("imported.error()") + " }\n";
      step += "f." + out + " = *imported;\n";
    }
    for (std::size_t i = 0; i < fn.parameters.size(); ++i)
      if (cpu.symbols[fn.parameters[i]].type.tensor)
        step += "task.context().release(argument" + std::to_string(i) + "->allocated);\n";
    if (!spawned) {
      steps.push_back(step);
      return value;
    }
    const auto prefix = "cpuJob" + std::to_string(cpuEntryCount++);
    std::string members = "m::Runtime *runtime;", captures = "f.runtime", owned = "data.runtime";
    std::string validation = "!data.runtime";
    std::set<std::size_t> captured;
    for (const auto &input : inputs) {
      if (!captured.insert(input.slot).second)
        continue;
      const auto name = "v" + std::to_string(input.slot);
      members += valueStorage(input) + " " + name + ";";
      captures += "," + ref(input);
      owned += ",data." + name;
      if (input.kind == Value::Tensor)
        validation += " || !data." + name + " || data." + name + ".onDevice() || data." + name +
                      ".format()!=*m::storageFormat(" + elementName(input.element) + ") || data." +
                      name + ".shape().size()!=" + std::to_string(input.rank);
    }
    cpuEntries += "struct " + prefix + "Packet{" + members + "};\nstruct " + prefix + "Frame{" +
                  members + (tensorResult ? "m::Buffer" : scalarType(value.element)) +
                  " value{};};\n";
    cpuEntries += "bool " + prefix +
                  "Construct(Invocation &context,void *out,const Packet &packet){\n"
                  "const auto &data=*static_cast<const " +
                  prefix +
                  "Packet*>(packet.data);\n"
                  "if(" +
                  validation +
                  "){context.fault(3);return false;}\n"
                  "new(out) " +
                  prefix + "Frame{" + owned + ",{}};return true;}\n";
    cpuEntries += "void " + prefix + "Destroy(void *frame) noexcept {static_cast<" + prefix +
                  "Frame*>(frame)->~" + prefix + "Frame();}\n";
    cpuEntries += "void " + prefix + "Resume(Task &task){auto &f=*static_cast<" + prefix +
                  "Frame*>(task.frame());auto &r=*f.runtime; "
                  "(void)r;\nif(task.cancelled()) return;\n" +
                  step + "task.complete(&f.value); }\n";
    cpuEntries += "const TaskEntry " + prefix + "Entry{1," +
                  std::to_string(0x4b414c4350550000ULL + cpuEntryCount) + "ull,sizeof(" + prefix +
                  "Packet),alignof(" + prefix + "Packet),sizeof(" + prefix + "Frame),alignof(" +
                  prefix + "Frame)," + prefix + "Construct," + prefix + "Destroy," + prefix +
                  "Resume," +
                  (tensorResult ? "&m::Runtime::bufferLayout()"
                                : "numericScalarLayout(" + elementName(value.element) + ")") +
                  "};\n";
    auto taskField = field("TaskHandle");
    steps.push_back(prefix + "Packet packet{" + captures + "};\nf." + taskField + "=task.spawn(" +
                    activeGroup + "," + prefix + "Entry,{" + prefix +
                    "Entry.packetLayout,&packet,sizeof(packet),&task.context()});\n" +
                    admitted("f." + taskField));
    value.kind = Value::Task;
    value.scalarTask = !tensorResult;
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
      if ((input.kind != Value::Tensor && input.kind != Value::Scalar &&
           input.kind != Value::Boolean) ||
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
    value.scalarTask = !type.tensor;
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
  Value spawnNumericProgram(FunctionId function, const std::vector<Value> &inputs, Value target,
                            SourceSpan span) {
    admitGpuTypes(function);
    const auto &fn = cpu.functions.at(function);
    auto found = numericPrograms.find(function);
    if (found == numericPrograms.end()) {
      auto program = emitNumericProgram(cpu, function, arithmetic_);
      if (!program)
        throw Rejected{program.error()};
      found = numericPrograms.emplace(function, std::move(*program)).first;
    }
    const auto &program = found->second;
    std::vector<Value> tensors, scalars;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto &type = cpu.symbols.at(fn.parameters.at(i)).type;
      const auto &value = inputs[i];
      completed(value, span);
      if (type.builtin != BuiltinValue::Numeric || value.element != type.element ||
          value.kind != (type.tensor ? Value::Tensor : Value::Scalar))
        reject(span, "typed numerical program argument mismatch");
      if (type.tensor) {
        if (!value.gpu || value.device != target.device || value.rank != type.shape.size())
          reject(span,
                 "typed numerical program requires explicit matching device tensor transfers");
        for (unsigned axis = 0; axis < type.shape.size(); ++axis)
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
    const auto [entry, inserted] = entries.emplace(function, entries.size());
    (void)inserted;
    const auto base = "kernel" + std::to_string(entry->second);
    auto out = field("TaskHandle");
    Value result{Value::Task, next - 1, true, target.device};
    result.scalarTask = !program.type.tensor;
    result.element = program.type.element;
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
      if (op.kind == NumericProgramKind::Map)
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
      const auto path = base + (i ? "-stage" + std::to_string(i) : "") + ".metallib";
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
      if (kind == NumericProgramKind::Map)
        code += ",m::ElementwiseSignature{" + std::to_string(kernel.tensorTypes.size()) + "," +
                std::to_string(kernel.scalarTypes.size()) +
                (kernel.indirect ? ",m::ElementwiseBinding::ArgumentBuffer}," : "},") +
                schema(kernel.tensorTypes) + "," + schema(kernel.scalarTypes) + "," +
                indices(kernel.reads, "std::uint32_t") + "," +
                indices(kernel.scalarBuffers, "std::uint8_t");
      code += ");if(!" + name + "){" + error(name + ".error()") + "}\n";
    }
    constexpr std::array names{"Map",   "Reduce",     "Matmul", "Branch",      "Jump",
                               "Alias", "EqualShape", "Extent", "MatrixShape", "ShapeFault"};
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
            "u,code," + std::to_string(program.result) + "u," + elementName(program.type.element) +
            "," + (result.scalarTask ? "true" : "false") + ");if(!prepared){" +
            error("prepared.error()") + "}f." + executable + "=*prepared;\n}\nf." + out +
            "=r.executeNumericProgram(task," + activeGroup + ",f." + executable +
            ",std::array<m::Buffer," + std::to_string(tensors.size()) + ">{";
    for (const auto &value : tensors)
      code += ref(value) + ",";
    code += "},std::array<std::uint64_t," + std::to_string(captureCount) + ">{" + captureList +
            "});\n" + admitted("f." + out);
    steps.push_back(std::move(code));
    return result;
  }
  Value spawnNumeric(FunctionId function, const std::vector<Value> &inputs, Value target,
                     SourceSpan span) {
    admitGpuTypes(function);
    const auto &fn = cpu.functions.at(function);
    if (numericPrograms.contains(function))
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
    bool mixed = false;
    bool conditional = false, primitive = false, transform = false;
    std::set<FunctionId> inspected;
    std::vector<FunctionId> pending{function};
    while (!pending.empty()) {
      auto id = pending.back();
      pending.pop_back();
      if (!inspected.insert(id).second)
        continue;
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
    bool selectedProgram = transform;
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
  Value lowerPlan(const continuation::Plan &plan, FunctionId entryId) {
    using namespace continuation;
    for (const auto &type : plan.slots)
      if (type.array || type.errorRecord)
        reject({}, "mixed Metal continuations currently require tensors and "
                   "TaskError rather than "
                   "array/application-error carriers");
    std::map<Slot, Value> values;
    std::map<Slot, unsigned> payloadWrites;
    std::map<std::size_t, std::size_t> joins;
    std::map<Slot, std::size_t> gpuCalls;
    for (std::size_t i = 0; i < plan.code.size(); ++i)
      if (plan.code[i].code == Code::Join)
        joins.emplace(plan.code[i].group, i);
    for (std::size_t i = 0; i < plan.groups; ++i)
      plannedGroups.push_back("f." + field("GroupHandle"));
    auto shape = [](Value &value, const ValueType &type) {
      if (type.builtin == BuiltinValue::Numeric)
        value.element = type.element;
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
      Value value{type.task ? Value::Task : type.tensor ? Value::Tensor : Value::Scalar};
      shape(value, type);
      std::string storage = type.task     ? "TaskHandle"
                            : type.tensor ? "m::Buffer"
                                          : scalarType(type.element);
      if (type.builtin == BuiltinValue::Device) {
        value.kind = Value::Target;
      } else {
        if (!type.task && type.builtin == BuiltinValue::Bool) {
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
      value.scalarTask = type.task && !type.tensor;
      if (type.fallible) {
        field("Failure");
        value.failure = next - 1;
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
      if (output.failure)
        code +=
            failureRef(output) + "=" + (input.failure ? failureRef(input) : "Failure{}") + ";\n";
      return code;
    };
    // Backend kernels can prove a result shape more precisely than its public
    // type. Carry those facts through values, but meet all payload-producing
    // branch writes rather than letting the last arm dictate a merge shape.
    auto write = [&](Slot slot, Value input) {
      auto output = get(slot);
      const auto &type = plan.slots.at(slot);
      if (type.tensor) {
        const bool first = payloadWrites[slot]++ == 0;
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
          bind(op.output, resumable.at(op.target)
                              ? spawnHelper(op.target, args, target, span)
                              : spawn(cpuFunction(op.target, span), args, target, span));
        }
        break;
      }
      case Code::Open: {
        auto group = plannedGroups.at(op.group);
        steps.push_back(group + "=task.openGroup(" +
                        (op.target == SIZE_MAX ? "" : plannedGroups.at(op.target)) + ");\n" +
                        check(group));
        break;
      }
      case Code::Await: {
        auto output = write(op.output, args.at(0)), input = args.at(0);
        if (auto found = gpuCalls.find(op.args[0]); found != gpuCalls.end())
          gpuAwaitSites.push_back({i, found->second});
        steps.push_back("if(!task.await(" + ref(input) + ")) return;\n" + failureRef(output) +
                        "=task.failure(" + ref(input) + ");\nif(" + failureRef(output) +
                        ".kind==TaskFailure::None){\n" +
                        (input.scalarTask
                             ? "if(!readScalar<" + elementName(input.element) + ">(task,"
                             : "if(!readBuffer(task,") +
                        ref(input) + "," + ref(output) + ")) return;\n}\n");
        break;
      }
      case Code::Try:
      case Code::Fail: {
        const auto failure = op.code == Code::Fail ? ref(args[0]) : failureRef(args[0]);
        std::string code =
            op.code == Code::Try ? "if(" + failure + ".kind!=TaskFailure::None){\n" : "{\n";
        if (op.group != SIZE_MAX)
          code += "task.failGroup(" + plannedGroups.at(op.group) + "," + failure + ");\n";
        else
          code += failureRef(get(op.target)) + "=" + failure + ";\n";
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
        steps.push_back("if(" + failureRef(args[0]) + ".kind==TaskFailure::None){\n" +
                        copy(output, args[0]) + jump(op.target) + "}\n" + ref(error) + "=" +
                        failureRef(args[0]) + ";\n" + jump(op.jump));
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
        const auto tag =
            subject.kind == Value::Error ? "unsigned(" + ref(subject) + ".kind)" : ref(subject);
        steps.push_back("if(" + tag + "!=" + std::to_string(op.target) + "){\n" +
                        (op.jump == SIZE_MAX ? error("m::Error::Invalid") : jump(op.jump)) + "}\n");
        break;
      }
      case Code::Jump:
        steps.push_back(jump(op.jump));
        break;
      case Code::Field: {
        auto output = get(op.output);
        const auto subject = args.at(0);
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
        steps.push_back(ref(output) + "=" + code + ";\n");
        break;
      }
      case Code::Join: {
        const auto group = plannedGroups.at(op.group);
        auto output = write(op.output, args.at(0));
        steps.push_back(check("task.close(" + group + ")"));
        steps.push_back("if(!task.join(" + group + ")) return;\n" + failureRef(output) +
                        "=task.failure(" + group + ");\nif(" + failureRef(output) +
                        ".kind==TaskFailure::None){\n" + payloadCopy(output, args.at(0)) + "}\n");
        break;
      }
      case Code::MapInit:
      case Code::MapCheck:
      case Code::MapLoad:
      case Code::MapStore:
      case Code::MapAdvance:
      case Code::MapSuccess:
      case Code::ReleaseGroup:
      case Code::CallableTarget:
      case Code::CallableCaptures:
      case Code::CallableSyncGuard:
      case Code::InvalidCallable:
      case Code::FunctionReference:
      case Code::FunctionInvoke:
      case Code::PropagateGroup:
      case Code::SynchronousCall:
        reject(span, "function values require the CPU backend");
      case Code::Label:
        break;
      case Code::Return: {
        auto result = args.at(0);
        steps.push_back((result.failure
                             ? "if(" + failureRef(result) + ".kind!=TaskFailure::None){task.fail(" +
                                   failureRef(result) + ");return;}\n"
                             : "") +
                        "task.complete(&" + ref(result) + ");return;\n");
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
  std::string helperPacket(FunctionId id) const {
    std::string code = "namespace " + helperName(id) +
                       " { struct PacketData { m::Runtime *runtime; const char *directory;";
    const auto &fn = placement.functions.at(id);
    for (std::size_t n = 0; n < fn.parameters.size(); ++n) {
      const auto &type = placement.symbols.at(fn.parameters[n]).type;
      code += (type.tensor                          ? "m::Buffer"
               : type.builtin == BuiltinValue::Bool ? "bool"
                                                    : scalarType(type.element)) +
              " arg" + std::to_string(n) + ";";
    }
    return code + "}; extern const TaskEntry entry; }\n";
  }
  std::string helperBody(FunctionId id) const {
    const auto &fn = placement.functions.at(id);
    if (fn.resultType.builtin == BuiltinValue::Bool)
      reject(fn.declaration, "Boolean coordinating task results are not "
                             "implemented by the Metal bridge");
    std::ostringstream out;
    out << "namespace " << helperName(id)
        << " {\nstruct Frame {m::Runtime *runtime; std::string directory; "
           "unsigned pc=1;\n";
    for (const auto &field : fields)
      out << field << '\n';
    out << "};\nbool construct(Invocation &context,void *out,const Packet "
           "&packet){\n"
           "const auto &data=*static_cast<const PacketData*>(packet.data);\n"
           "if(!data.runtime || !data.directory";
    for (std::size_t n = 0; n < plannedParameters.size(); ++n) {
      const auto &type = placement.symbols.at(fn.parameters[n]).type;
      if (type.tensor) {
        auto arg = "data.arg" + std::to_string(n);
        out << " || !" << arg << " || " << arg << ".onDevice() || " << arg
            << ".format()!=*m::storageFormat(" << elementName(type.element) << ") || " << arg
            << ".shape().size()!=" << type.shape.size();
      }
    }
    out << "){context.fault(3);return false;}\n"
           "auto &f=*new(out) Frame{data.runtime,data.directory}; (void)f;\n";
    for (std::size_t n = 0; n < plannedParameters.size(); ++n)
      out << ref(plannedParameters[n]) << "=data.arg" << n << ";\n";
    out << "return true;}\nvoid destroy(void *p) noexcept "
           "{static_cast<Frame*>(p)->~Frame();}\n"
           "void resume(Task &task){auto "
           "&f=*static_cast<Frame*>(task.frame()); auto "
           "&r=*f.runtime; (void)r;\n"
           "if(task.cancelled())return;\nfor(;;){switch(f.pc){\n";
    for (std::size_t i = 0; i < steps.size(); ++i)
      out << "case " << i + 1 << ": {\n"
          << steps[i] << "f.pc=" << i + 2 << ";\n} [[fallthrough]];\n";
    out << "default:task.fault(3);return;}}}\n"
           "const TaskEntry entry{1,"
        << 0x4b414c52534d0000ULL + id
        << "ull,sizeof(PacketData),alignof(PacketData),"
           "sizeof(Frame),alignof(Frame),construct,destroy,resume,"
        << (fn.resultType.tensor
                ? "&m::Runtime::bufferLayout()"
                : "numericScalarLayout(" + elementName(fn.resultType.element) + ")")
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
  explicit Compiler(Module module, ArithmeticMode arithmetic, bool testCpuExports)
      : arithmetic_(arithmetic), testCpuExports_(testCpuExports), syntax(std::move(module)) {}
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
      if (fn.result->array)
        reject(fn.result->span, "mixed Metal array results require the CPU backend");
      if (fn.result->tensor)
        tensorType(*fn.result);
      else if (!resolveNumeric(text(fn.result->name)) && text(fn.result->name) != "Bool")
        reject(fn.result->span, "mixed source results require float32, Bool or rank-one through "
                                "rank-eight float32 tensors");
      for (const auto &p : fn.parameters) {
        if (p.type.array)
          reject(p.type.span, "mixed Metal array parameters require the CPU backend");
        if (p.type.tensor)
          tensorType(p.type);
        else if (!resolveNumeric(text(p.type.name)) && text(p.type.name) != "Bool")
          reject(p.type.span, "mixed source parameters require float32, Bool "
                              "or rank-one through "
                              "rank-eight float32 tensors");
      }
      if (resumable[i])
        continue;
      cpuFunctions.emplace(name, hostModule.functions.size());
      cpuExports.push_back(name);
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
          return found->second == *main || resumable[found->second];
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
      declareCpuFunction(i);
    const auto &fn = syntax.functions[*main];
    if (!fn.parameters.empty() || !fn.result || !fn.taskError)
      reject(fn.span, "Metal main requires no parameters, a float32 or tensor "
                      "result, and ! TaskError");
    bool scalarResult = !fn.result->tensor;
    if (scalarResult && !resolveNumeric(text(fn.result->name)))
      reject(fn.span, "Metal main requires float32 or a rank-one through "
                      "rank-eight float32 tensor");
    if (!scalarResult)
      tensorType(*fn.result);
    const auto mainElement = placement.functions.at(*main).resultType.element;
    const auto mainWidth = describeNumeric(mainElement)->bytes();
    auto planned = continuation::plan(placement, {.preserveCalls = true, .entry = *main});
    if (!planned)
      reject(fn.span, planned.error());
    auto result = lowerPlan(*planned->plans[*main], *main);
    (void)result;
    // Compile each reachable resumable child once. Keep the main frame's state
    // separate while numeric exports and shader entries remain module-wide.
    auto mainFields = std::move(fields), mainSteps = std::move(steps),
         mainGroups = std::move(plannedGroups);
    auto mainPC = std::move(plannedPC);
    auto mainSites = std::move(gpuAwaitSites);
    auto mainParameters = std::move(plannedParameters);
    const auto mainNext = next;
    std::string helperDeclarations, helperBodies;
    for (FunctionId id = 0; id < planned->plans.size(); ++id) {
      if (id == *main || planned->plans[id]->code.empty())
        continue;
      fields.clear();
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
          outputs[name + (shader.nativeMetal.empty() ? ".mlir" : ".native.metal")] =
              shader.nativeMetal.empty() ? shader.mlir : shader.nativeMetal;
        }
        continue;
      }
      if (auto found = numericShaders.find(function); found != numericShaders.end()) {
        outputs[base + ".mlir"] = found->second.mlir;
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
    if (testCpuExports_ || !usedCpuFunctions.empty()) {
      auto exports = cpuExports;
      for (std::size_t i = 0; i < exports.size(); ++i)
        if (!testCpuExports_ && !usedCpuFunctions.contains(i))
          exports[i].clear();
      auto lowered = lowerNative(cpu, {}, exports, arithmetic_);
      if (!lowered)
        reject(fn.span, lowered.error());
      outputs["cpu.mlir"] = lowered->mlir;
      outputs["cpu.ll"] = lowered->llvm;
    }
    std::ostringstream host;
    host << R"(#include "Metal.h"
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
 )" << declarations
         << helperDeclarations << cpuEntries;
    if (scalarResult)
      host << "const ValueLayout &scalarLayout=*numericScalarLayout(" + elementName(mainElement) +
                  ");\n";
    host << R"(struct PacketData { m::Runtime *runtime; const char *directory; };
struct Frame { m::Runtime *runtime; std::string directory; unsigned pc=1;
)";
    for (auto &f : fields)
      host << "  " << f << '\n';
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
    host << R"(bool construct(Invocation &, void *out, const Packet &p) {
  auto &data=*static_cast<const PacketData *>(p.data);
  new(out) Frame{data.runtime,data.directory}; return true;
}
void destroy(void *p) noexcept { static_cast<Frame *>(p)->~Frame(); }
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
void resume(Task &task) {
  auto &f=*static_cast<Frame *>(task.frame()); [[maybe_unused]] auto &r=*f.runtime;
  for (;;) { switch(f.pc) {
)";
    for (std::size_t i = 0; i < steps.size(); ++i)
      host << "case " << i + 1 << ": {\n"
           << steps[i] << "f.pc=" << i + 2 << ";\n} [[fallthrough]];\n";
    host << "default: task.fault(3); return;\n} }\n}\n";
    host
        << R"(const TaskEntry entry{1,0x4b414c47505501,sizeof(PacketData),alignof(PacketData),sizeof(Frame),alignof(Frame),construct,destroy,resume,)"
        << (scalarResult ? "&scalarLayout" : "&m::Runtime::bufferLayout()") << "};\n"
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
    if (scalarResult)
      host << scalarType(mainElement)
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
    if (scalarResult) {
      host << "std::printf(\"%0*llx\\n\"," << mainWidth * 2
           << ",static_cast<unsigned long long>(std::bit_cast<std::uint" << mainWidth * 8
           << "_t>(value)));\n";
    } else {
      host << R"(    auto *value=std::launder(reinterpret_cast<m::Buffer *>(slot));
    auto cpu=runtime->toCpuTyped<)"
           << elementName(mainElement) << "," << fn.result->dimensions.size()
           << R"(>(context,*value);
    if (!cpu) status=1;
    else { std::printf("["); for (std::uint64_t i=0;i<value->bytes()/)"
           << mainWidth << R"(;++i) {
      std::uint64_t bits=0; std::memcpy(&bits,cpu->aligned+i,)"
           << mainWidth << R"(); std::printf(i ? ",%0*llx" : "%0*llx",)" << mainWidth * 2
           << R"(,static_cast<unsigned long long>(bits));
    } std::puts("]"); context.release(cpu->allocated); }
    value->~Buffer();)";
    }
    host << R"(
  }
  auto s=runtime->stats(); auto t=runner->stats();
  if (s.bytes || s.metalBuffers || s.hostBackings || s.deviceBackings || s.inFlight || s.stagingBuffers || s.argumentBytes || s.argumentPacks || t.records || t.groups || t.externalPending || t.retainedBytes) return 1;
  return status;
}
)";
    outputs["host.cpp"] = host.str();
    std::string manifest = "kernel_count=" + std::to_string(entries.size()) + "\n";
    for (auto [function, index] : entries) {
      if (auto found = numericPrograms.find(function); found != numericPrograms.end()) {
        const auto prefix = "kernel" + std::to_string(index) + ".";
        manifest +=
            prefix + "numeric_program_instructions=" + std::to_string(found->second.code.size()) +
            "\n" + prefix +
            "numeric_program_stages=" + std::to_string(found->second.kernels.size()) + "\n" +
            prefix +
            "numeric_type=" + std::string(describeNumeric(found->second.type.element)->name) + "\n";
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
    outputs["manifest.txt"] = std::move(manifest);
    return outputs;
  }
};
} // namespace
int kalandra::compileMetalCommand(int argc, char **argv) {
  const auto usage = [] {
    std::cerr << "usage: kalandra-metal-source <source.k | --project "
                 "manifest.toml> -o directory "
                 "[--arithmetic hardware|software|hardware-with-software-fallback] "
                 "[--matmul-policy optimized|fixed-tree] "
                 "[--package name=path.k ...] [--build "
                 "[-O0|-O2|--sanitize|--thread-sanitize]]\n";
    return 2;
  };
  bool build = false, testCpuExports = false;
  ArithmeticMode arithmetic = ArithmeticMode::Hardware;
  bool selectedArithmetic = false;
  MatmulPolicy matmul = MatmulPolicy::Optimized;
  bool selectedMatmul = false;
  std::string variant = "debug", sourcePath, projectPath;
  std::filesystem::path directory;
  std::optional<NativeProject> project;
  std::vector<ModulePackage> packages;
  bool selected = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view option = argv[i];
    if (option == "-o" || option == "--project" || option == "--package") {
      if (++i >= argc || std::string_view(argv[i]).empty())
        return usage();
      if (option == "-o") {
        if (!directory.empty())
          return usage();
        directory = argv[i];
      } else if (option == "--project") {
        if (!projectPath.empty())
          return usage();
        projectPath = argv[i];
      } else {
        auto package = parseModulePackage(argv[i]);
        if (!package) {
          std::cerr << package.error() << '\n';
          return 2;
        }
        packages.push_back(std::move(*package));
      }
    } else if (option == "--arithmetic" && !selectedArithmetic) {
      if (++i >= argc)
        return usage();
      auto parsed = parseArithmeticMode(argv[i]);
      if (!parsed)
        return usage();
      arithmetic = *parsed;
      selectedArithmetic = true;
    } else if (option == "--matmul-policy" && !selectedMatmul) {
      if (++i >= argc)
        return usage();
      auto parsed = parseMatmulPolicy(argv[i]);
      if (!parsed)
        return usage();
      matmul = *parsed;
      selectedMatmul = true;
    } else if (option == "--test-cpu-exports" && !testCpuExports) {
      testCpuExports = true;
    } else if (option == "--build" && !build)
      build = true;
    else if (!selected && (option == "-O0" || option == "-O2" || option == "--sanitize" ||
                           option == "--thread-sanitize")) {
      selected = true;
      variant = option == "-O2"          ? "optimized"
                : option == "--sanitize" ? "sanitized"
                : option == "-O0"        ? "debug"
                                         : "thread-sanitized";
    } else if (!option.starts_with('-') && sourcePath.empty())
      sourcePath = option;
    else
      return usage();
  }
  if (directory.empty() || (selected && !build) || (!sourcePath.empty() && !projectPath.empty()))
    return usage();
  if (arithmetic == ArithmeticMode::Software)
    matmul = MatmulPolicy::FixedTree;
  if (!projectPath.empty()) {
    auto loaded = loadNativeProject(projectPath);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    project = std::move(*loaded);
    sourcePath = project->entry;
    packages.insert(packages.end(), project->packages.begin(), project->packages.end());
  }
  if (sourcePath.empty())
    return usage();
  std::optional<InstalledNativeProject> managed;
  if (project && !project->dependencies.empty()) {
    auto loaded = loadInstalledNativeProject(*project);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    managed = std::move(*loaded);
    managed->nodes[0].localPackages = packages;
  }
  auto parsed =
      managed ? loadPackageModules(managed->nodes) : loadModules(sourcePath, {}, {}, packages);
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  auto source = parsed->module.source;
  try {
    if (!parsed->ok()) {
      for (auto &d : parsed->diagnostics)
        std::cerr << renderDiagnostic(*source, d);
      return 1;
    }
    auto outputs = Compiler(std::move(parsed->module), arithmetic, testCpuExports).run();
    outputs["manifest.txt"] += "matmul_policy=" + std::string(matmulPolicyName(matmul)) + "\n";
    if (matmul == MatmulPolicy::Optimized)
      outputs["THIRD_PARTY_NOTICES.txt"] = kalandraSteelLicense;
    if (outputs.contains("cpu.ll") && (variant == "sanitized" || variant == "thread-sanitized")) {
      llvm::LLVMContext context;
      llvm::SMDiagnostic diagnostic;
      auto module = llvm::parseAssemblyString(outputs.at("cpu.ll"), diagnostic, context);
      if (!module)
        throw std::runtime_error("cannot prepare CPU sanitizer attributes");
      for (auto &function : *module)
        if (!function.isDeclaration())
          function.addFnAttr(variant == "sanitized" ? llvm::Attribute::SanitizeAddress
                                                    : llvm::Attribute::SanitizeThread);
      outputs["cpu.ll"].clear();
      llvm::raw_string_ostream stream(outputs["cpu.ll"]);
      module->print(stream, nullptr);
    }
    std::vector<std::string> paths;
    for (const auto &[name, contents] : outputs) {
      (void)contents;
      paths.push_back(name);
      if (build && name.starts_with("kernel") && name.ends_with(".mlir"))
        for (const auto *suffix : {".lowered.mlir", ".converted.mlir", ".kernel.mlir", ".spv",
                                   ".metal", ".air", ".metallib"})
          paths.push_back(std::filesystem::path(name).stem().string() + suffix);
      if (build && name.starts_with("kernel") && name.ends_with(".native.metal")) {
        const auto base = name.substr(0, name.size() - std::string(".native.metal").size());
        paths.push_back(base + ".air");
        paths.push_back(base + ".metallib");
      }
    }
    if (build)
      paths.push_back("program");
    for (const auto &name : paths) {
      auto target = directory / name;
      if (project) {
        auto allowed = protectNativePackageOutput(*project, target.string());
        if (!allowed)
          throw std::runtime_error(allowed.error());
      }
      std::error_code ec;
      auto protect = [&](const std::filesystem::path &input) {
        std::error_code first, second;
        const bool equivalent = std::filesystem::equivalent(target, input, ec);
        const auto a = std::filesystem::weakly_canonical(target, first);
        const auto b = std::filesystem::weakly_canonical(input, second);
        if (equivalent || (!first && !second && a == b))
          throw std::runtime_error("source and output paths must be distinct");
      };
      protect(source->path());
      for (const auto &part : source->parts())
        protect(part.source->path());
      if (project) {
        protect(project->manifest);
        if (project->libraryEntry)
          protect(*project->libraryEntry);
      }
      if (std::filesystem::is_symlink(std::filesystem::symlink_status(target)))
        throw std::runtime_error("refusing symlink output");
    }
    std::filesystem::create_directories(directory);
    for (auto &[name, contents] : outputs) {
      llvm::SmallString<256> temporary;
      int descriptor = -1;
      auto target = directory / name;
      if (auto ec = llvm::sys::fs::createUniqueFile(target.string() + ".tmp-%%%%%%", descriptor,
                                                    temporary))
        throw std::runtime_error(ec.message());
      llvm::FileRemover remove(temporary);
      llvm::raw_fd_ostream file(descriptor, true);
      file << contents;
      file.close();
      if (file.has_error())
        throw std::runtime_error("cannot write compiler output");
      std::filesystem::rename(std::string(temporary), target);
    }
    if (build)
      buildMetalProgram(directory, outputs, variant, arithmetic, matmul);
    return 0;
  } catch (const Rejected &e) {
    std::cerr << renderDiagnostic(*source, e.diagnostic);
  } catch (const std::exception &e) {
    std::cerr << "Metal source compilation: " << e.what() << '\n';
  }
  return 1;
}
