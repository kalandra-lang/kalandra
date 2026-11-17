#include "WorkgroupPlan.h"
#include "../gpu-source/DeviceErrorLayout.h"
#include <map>
#include <set>

namespace kalandra {
namespace {
struct Rejected {
  Diagnostic diagnostic;
};
struct Value {
  WorkgroupCapture capture;
  std::vector<Value> elements;
  std::optional<FunctionId> function;
  std::shared_ptr<WorkgroupPlan> dispatch;
};
struct Frame {
  std::map<SymbolId, Value> symbols;
  std::map<ValueId, Value> cache;
  std::optional<std::pair<std::size_t, Value>> returned;
};
class Planner {
  const TypedModule &module;
  WorkgroupLaunch launch;
  std::set<FunctionId> active;
  std::size_t steps = 0;
  std::size_t launches = 0;
  [[noreturn]] void reject(ValueId id, const char *message) const {
    throw Rejected{{"G001", message, module.expressions.at(id).span}};
  }
  Value evaluate(Frame &frame, ValueId id) {
    if (++steps > 300000)
      reject(id, "cooperative launch planning exceeds its work budget");
    if (frame.returned)
      return frame.returned->second;
    if (auto found = frame.cache.find(id); found != frame.cache.end())
      return found->second;
    const auto &node = module.expressions.at(id);
    Value out;
    out.capture.type = node.type;
    if (auto read = std::get_if<SymbolRead>(&node.operation)) {
      auto found = frame.symbols.find(read->symbol);
      if (found == frame.symbols.end())
        reject(id, "unbound cooperative launch capture");
      out = found->second;
    } else if (auto shape = std::get_if<CollectionQuery>(&node.operation)) {
      auto input = evaluate(frame, shape->input);
      if (!input.capture.tensor)
        reject(id, "cooperative launch shapes require a resident tensor");
      const auto count = input.capture.type.shape.size();
      if (shape->kind == CollectionQueryKind::Length)
        out.capture.scalar = {{WorkgroupShapeAction::Extent, 0, *input.capture.tensor, 0}};
      else
        for (std::size_t axis = 0; axis < count; ++axis) {
          Value extent;
          extent.capture.type.element = NumericType::Int64;
          extent.capture.scalar = {{WorkgroupShapeAction::Extent, 0, *input.capture.tensor,
                                    static_cast<std::uint32_t>(axis)}};
          out.elements.push_back(std::move(extent));
        }
    } else if (auto array = std::get_if<TensorLiteral>(&node.operation)) {
      for (auto value : array->elements)
        out.elements.push_back(evaluate(frame, value));
    } else if (auto index = std::get_if<ArrayIndex>(&node.operation)) {
      auto input = evaluate(frame, index->input), at = evaluate(frame, index->index);
      if (at.capture.scalar.size() != 1 ||
          at.capture.scalar[0].action != WorkgroupShapeAction::Constant ||
          at.capture.scalar[0].value >= input.elements.size())
        reject(id, "cooperative launch metadata indexing requires a valid constant axis");
      out = input.elements[at.capture.scalar[0].value];
    } else if (auto reference = std::get_if<FunctionReference>(&node.operation)) {
      out.function = reference->function;
      for (auto value : reference->captures)
        out.elements.push_back(evaluate(frame, value));
    } else if (auto call = std::get_if<FunctionCall>(&node.operation)) {
      std::vector<Value> arguments;
      for (auto value : call->arguments)
        arguments.push_back(evaluate(frame, value));
      out = invoke(call->function, std::move(arguments), id);
    } else if (auto operation = std::get_if<TaskOperation>(&node.operation);
               operation && operation->prefix == TaskPrefix::Try && !node.type.task) {
      out = evaluate(frame, operation->operand);
      if (!out.dispatch)
        reject(id, "cooperative launch prelude cannot discard an expected-error operation");
    } else if (auto boundary = std::get_if<WhileOperation>(&node.operation);
               boundary && boundary->functionBoundary) {
      suite(frame, boundary->initial);
      suite(frame, boundary->iteration);
      suite(frame, boundary->body);
      if (!frame.returned || frame.returned->first != boundary->identity)
        reject(id, "cooperative launch wrapper has unresolved control flow");
      out = frame.returned->second;
      frame.returned.reset();
    } else if (auto transfer = std::get_if<LoopTransferOperation>(&node.operation)) {
      if (transfer->continuing || !transfer->value)
        reject(id, "cooperative launch planning requires a returned dispatch");
      out = evaluate(frame, *transfer->value);
      frame.returned = {{transfer->loop, out}};
    } else if (auto dispatch = std::get_if<WorkgroupOperation>(&node.operation);
               dispatch && dispatch->metadata.action == WorkgroupAction::Dispatch) {
      auto shape = evaluate(frame, dispatch->arguments[0]);
      auto callback = evaluate(frame, dispatch->arguments[2]);
      if (!callback.function || shape.elements.size() != dispatch->groupShape.size())
        reject(id, "cooperative launch has unresolved callback or shape provenance");
      WorkgroupPlan plan{id, *callback.function, launch, {}, {}};
      plan.launch.groupShape = dispatch->groupShape;
      for (const auto &extent : shape.elements) {
        if (extent.capture.type.element != NumericType::Int64 || extent.capture.scalar.empty())
          reject(id, "cooperative launch extent must be host-known i64 metadata");
        plan.launch.shape.push_back(extent.capture.scalar);
      }
      for (auto &capture : callback.elements) {
        if ((!capture.capture.tensor && capture.capture.scalar.empty()) ||
            capture.capture.type.array || capture.capture.type.task ||
            (capture.capture.type.builtin != BuiltinValue::Numeric &&
             capture.capture.type.builtin != BuiltinValue::Bool))
          reject(id, "cooperative callbacks capture numeric scalars and resident numeric tensors");
        plan.captures.push_back(std::move(capture.capture));
      }
      auto proof = analyzeWorkgroup(module, id);
      if (!proof)
        throw Rejected{proof.error()};
      plan.proof = std::move(*proof);
      plan.launch.scratchBytes = plan.proof.scratchBytes;
      if (++launches != 1)
        reject(id, "cooperative entry orchestration requires one returned dispatch");
      out.dispatch = std::make_shared<WorkgroupPlan>(std::move(plan));
    } else if (auto binary = std::get_if<ScalarBinary>(&node.operation)) {
      if (launches)
        reject(id, "checked work after cooperative dispatch requires a separate graph stage");
      auto left = evaluate(frame, binary->left), right = evaluate(frame, binary->right);
      if (node.type.element != NumericType::Int64 || left.capture.scalar.empty() ||
          right.capture.scalar.empty())
        reject(id, "cooperative launch arithmetic requires host-known i64 metadata");
      const auto operation = binary->operation;
      const auto action = operation == TokenKind::Plus    ? WorkgroupShapeAction::Add
                          : operation == TokenKind::Minus ? WorkgroupShapeAction::Subtract
                          : operation == TokenKind::Star  ? WorkgroupShapeAction::Multiply
                          : operation == TokenKind::Slash ? WorkgroupShapeAction::Divide
                                                          : WorkgroupShapeAction::Remainder;
      if (operation != TokenKind::Plus && operation != TokenKind::Minus &&
          operation != TokenKind::Star && operation != TokenKind::Slash &&
          operation != TokenKind::Percent)
        reject(id, "unsupported cooperative launch metadata arithmetic");
      out.capture.scalar = std::move(left.capture.scalar);
      out.capture.scalar.insert(out.capture.scalar.end(), right.capture.scalar.begin(),
                                right.capture.scalar.end());
      out.capture.scalar.push_back({action});
      launch.checked.push_back(out.capture.scalar);
    } else if (auto unary = std::get_if<ScalarUnary>(&node.operation)) {
      if (launches)
        reject(id, "checked work after cooperative dispatch requires a separate graph stage");
      out = evaluate(frame, unary->operand);
      if (node.type.element != NumericType::Int64 ||
          (unary->operation != TokenKind::Plus && unary->operation != TokenKind::Minus))
        reject(id, "unsupported cooperative launch unary operation");
      if (unary->operation == TokenKind::Minus) {
        out.capture.scalar.push_back({WorkgroupShapeAction::Negate});
        launch.checked.push_back(out.capture.scalar);
      }
    } else {
      std::optional<std::uint64_t> bits;
      std::visit(
          [&](const auto &operation) {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<T, Float32Literal> || std::is_same_v<T, Float64Literal>)
              bits = operation.bits;
            else if constexpr (std::is_same_v<T, Int32Literal> ||
                               std::is_same_v<T, UInt32Literal> ||
                               std::is_same_v<T, Int64Literal> || std::is_same_v<T, UInt64Literal>)
              bits = static_cast<std::uint64_t>(operation.value);
            else if constexpr (std::is_same_v<T, BoolLiteral>)
              bits = operation.value;
          },
          node.operation);
      if (!bits)
        reject(id, "cooperative launch prelude contains an unsupported host operation");
      out.capture.scalar = {{WorkgroupShapeAction::Constant, *bits}};
    }
    frame.cache[id] = out;
    return out;
  }
  void suite(Frame &frame, const std::vector<TypedStatement> &body) {
    for (const auto &statement : body) {
      if (frame.returned)
        break;
      auto value = evaluate(frame, statement.value);
      if (statement.binding)
        frame.symbols[*statement.binding] = value;
      frame.cache[statement.value] = std::move(value);
    }
  }
  Value invoke(FunctionId id, std::vector<Value> arguments, ValueId site) {
    if (active.size() >= 256 || !active.insert(id).second)
      reject(site, "recursive cooperative launch expansion");
    const auto &function = module.functions.at(id);
    if (arguments.size() != function.parameters.size())
      reject(site, "invalid cooperative helper arguments");
    Frame frame;
    for (std::size_t i = 0; i < arguments.size(); ++i)
      frame.symbols[function.parameters[i]] = arguments[i];
    suite(frame, function.body);
    auto out = evaluate(frame, function.result);
    if (frame.returned)
      reject(site, "unresolved cooperative wrapper return");
    active.erase(id);
    return out;
  }

public:
  explicit Planner(const TypedModule &input) : module(input) {}
  WorkgroupPlan run(FunctionId id) {
    const auto &function = module.functions.at(id);
    std::vector<Value> arguments;
    std::uint32_t scalars = 0, tensors = 0;
    for (auto symbol : function.parameters) {
      const auto &type = module.symbols[symbol].type;
      Value value;
      value.capture.type = type;
      if ((type.builtin != BuiltinValue::Numeric && type.builtin != BuiltinValue::Bool) ||
          type.array || type.task || type.fallible)
        reject(function.result, "cooperative launch captures require numeric scalars and tensors");
      if (type.tensor) {
        value.capture.tensor = tensors++;
        launch.inputRanks.push_back(type.shape.size());
      } else
        value.capture.scalar = {{WorkgroupShapeAction::Scalar, 0, scalars++, 0}};
      arguments.push_back(std::move(value));
    }
    auto result = invoke(id, std::move(arguments), function.result);
    if (result.dispatch && launches == 1) {
      result.dispatch->launch.checked = std::move(launch.checked);
      const auto &callback = module.functions.at(result.dispatch->callback).resultType;
      if (callback.fallible && callback.errorRecord != function.resultType.errorRecord)
        reject(function.result,
               "cooperative error export requires the callback's declared error family");
      if (function.resultType.fallible) {
        if (!function.resultType.errorRecord)
          reject(function.result,
                 "cooperative error export requires a declared application-error family");
        const std::array families{*function.resultType.errorRecord};
        auto layouts = planDeviceErrorPayloadLayouts(module, families);
        if (!layouts)
          reject(function.result,
                 "cooperative application errors require a device-compatible layout");
        const auto &layout = layouts->at(families.front());
        if (!layout.words || layout.words > 1024 || !layout.owned.empty())
          reject(function.result,
                 "cooperative application-error export requires a bounded inline numeric payload");
        result.dispatch->launch.errorWords = layout.words;
      }
      return std::move(*result.dispatch);
    }
    reject(function.result, "cooperative wrapper does not return its dispatch result");
  }
};
} // namespace
bool containsWorkgroupDispatch(const TypedModule &module, FunctionId root) {
  std::vector<FunctionId> pending{root};
  std::set<FunctionId> visited;
  while (!pending.empty()) {
    auto function = pending.back();
    pending.pop_back();
    if (!visited.insert(function).second)
      continue;
    for (auto id : module.functions.at(function).expressions) {
      const auto &operation = module.expressions[id].operation;
      if (auto workgroup = std::get_if<WorkgroupOperation>(&operation);
          workgroup && workgroup->metadata.action == WorkgroupAction::Dispatch)
        return true;
      if (auto call = std::get_if<FunctionCall>(&operation))
        pending.push_back(call->function);
    }
  }
  return false;
}
std::expected<WorkgroupPlan, Diagnostic> planWorkgroup(const TypedModule &module,
                                                       FunctionId function) {
  try {
    return Planner(module).run(function);
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}
} // namespace kalandra
