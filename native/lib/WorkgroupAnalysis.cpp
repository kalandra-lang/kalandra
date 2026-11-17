#include "kalandra/WorkgroupAnalysis.h"
#include "WorkgroupTypes.h"
#include "kalandra/WorkgroupGeometry.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace kalandra {
namespace {
struct Rejected {
  Diagnostic diagnostic;
};
struct Choice {
  std::string identity;
  bool uniform;
};
struct Value {
  enum class Kind { Data, Item, Scratch, Function, Collection } kind = Kind::Data;
  std::optional<__int128_t> integer;
  bool uniform = false;
  bool mutableBacking = false;
  std::size_t identity = SIZE_MAX;
  std::vector<Value> elements;
  // A checked fallible value carries either success data or one error value.
  // Expected failure does not become an implicit device fault.
  std::vector<Value> errors;
};
struct ControlExit {
  std::size_t loop;
  bool continuing;
  Value value;
};
struct Frame {
  std::map<SymbolId, Value> symbols;
  std::map<ValueId, Value> cache;
  std::optional<ControlExit> transfer;
  std::optional<Value> failure;
  bool uniformControl = true;
};
class Analyzer {
  const TypedModule &module;
  const WorkgroupAnalysisLimits &limits;
  const WorkgroupOperation &dispatch;
  SourceSpan declaration;
  WorkgroupGeometry geometry;
  WorkgroupAnalysis result;
  std::size_t steps = 0, depth = 0;
  std::map<std::string, std::size_t> scratchIds, barrierIds;
  std::vector<bool> effects;
  std::map<std::string, bool> uniformChoices, localChoices;
  std::string context, iteration;
  WorkgroupLane lane;
  std::vector<std::size_t> allocations;
  std::uint32_t laneId = 0;

  [[noreturn]] void reject(ValueId id, std::string message) const {
    throw Rejected{{"S087", std::move(message), id < module.expressions.size()
                                                ? module.expressions[id].span : declaration}};
  }
  void step(ValueId id) {
    if (++steps > limits.steps)
      reject(id, "cooperative control and ownership proof exceeds its work budget");
  }
  Value unknown(const ValueType &type, bool uniform) const {
    Value value;
    value.uniform = uniform;
    value.mutableBacking = type.array || type.builtin == BuiltinValue::Function ||
        (type.builtin == BuiltinValue::Record && !workgroupCapability(module, type));
    if (workgroupCapability(module, type) == StandardRecord::WorkItem)
      value.kind = Value::Kind::Item;
    else if (workgroupCapability(module, type) == StandardRecord::Shared)
      value.kind = Value::Kind::Scratch;
    else if (type.builtin == BuiltinValue::Function)
      value.kind = Value::Kind::Function;
    else if (type.tensor || type.array)
      value.kind = Value::Kind::Collection;
    return value;
  }
  static void invalidateBacking(Value &value) {
    if (!value.mutableBacking)
      return;
    value.uniform = false;
    if (value.kind == Value::Kind::Function) {
      for (auto &capture : value.elements)
        invalidateBacking(capture);
    } else
      value.elements.clear();
  }
  static void invalidateBackings(Frame &frame) {
    for (auto &[symbol, value] : frame.symbols) {
      (void)symbol;
      invalidateBacking(value);
    }
    frame.cache.clear();
  }
  bool mutatesBacking(FunctionId function) const {
    return function < module.functions.size() &&
        std::ranges::any_of(module.functions[function].parameters, [&](SymbolId parameter) {
          const auto &symbol = module.symbols[parameter];
          return symbol.mutableAccess || (symbol.owned && symbol.type.array);
        });
  }
  static Value integer(__int128_t number, bool uniform = true) {
    Value value;
    value.integer = number;
    value.uniform = uniform;
    return value;
  }
  std::string key(ValueId id, bool collective) const {
    return context + (collective ? iteration : "") + ":" + std::to_string(id);
  }
  bool choose(ValueId id, const Value &condition) {
    if (condition.integer)
      return *condition.integer != 0;
    const bool uniform = condition.uniform || geometry.localSize == 1;
    auto &choices = uniform ? uniformChoices : localChoices;
    const auto identity = key(id, true);
    if (auto found = choices.find(identity); found != choices.end())
      return found->second;
    throw Choice{identity, uniform};
  }
  void suite(Frame &frame, const std::vector<TypedStatement> &body) {
    for (const auto &statement : body) {
      if (frame.transfer || frame.failure)
        break;
      auto value = evaluate(frame, statement.value);
      if (statement.binding) {
        value.uniform &= frame.uniformControl;
        frame.symbols[*statement.binding] = value;
        frame.cache.clear();
        frame.cache[statement.value] = std::move(value);
      }
    }
  }
  Value invoke(FunctionId id, std::vector<Value> arguments, ValueId site, bool root = false) {
    step(site);
    if (id >= module.functions.size())
      reject(site, "cooperative callback has an invalid function target");
    const auto &function = module.functions[id];
    if (arguments.size() != function.parameters.size())
      reject(site, "cooperative callback has an unresolved capture layout");
    const bool capability = workgroupCapability(module, function.resultType).has_value() ||
        std::ranges::any_of(arguments, [](const Value &v) {
          return v.kind == Value::Kind::Item || v.kind == Value::Kind::Scratch;
        });
    if (!root && !effects[id] && !capability && !function.resultType.fallible) {
      // Opaque numerical helpers may allocate or consume a lane-local call/work
      // budget. Equal arguments alone do not establish uniform failure behavior.
      // Only a finite scalar leaf can be folded into a shared-address proof.
      const bool scalarLeaf = std::ranges::all_of(function.expressions, [&](ValueId expression) {
        const auto &operation = module.expressions[expression].operation;
        return std::holds_alternative<Int32Literal>(operation) ||
            std::holds_alternative<Int64Literal>(operation) ||
            std::holds_alternative<UInt32Literal>(operation) ||
            std::holds_alternative<UInt64Literal>(operation) ||
            std::holds_alternative<Float32Literal>(operation) ||
            std::holds_alternative<Float64Literal>(operation) ||
            std::holds_alternative<BoolLiteral>(operation) ||
            std::holds_alternative<SymbolRead>(operation) ||
            std::holds_alternative<ScalarUnary>(operation) ||
            std::holds_alternative<ScalarBinary>(operation) ||
            std::holds_alternative<ScalarCompare>(operation) ||
            std::holds_alternative<NumericConvert>(operation) ||
            std::holds_alternative<ConditionalOperation>(operation) ||
            std::holds_alternative<LoopTransferOperation>(operation) ||
            (std::holds_alternative<WhileOperation>(operation) &&
             std::get<WhileOperation>(operation).functionBoundary);
      });
      const bool known = std::ranges::all_of(arguments, [](const Value &v) {
        return v.integer.has_value();
      });
      if (!scalarLeaf || !known)
        return unknown(function.resultType, scalarLeaf &&
            std::ranges::all_of(arguments, [](const Value &v) { return v.uniform; }));
    }
    if (++depth > limits.callDepth)
      reject(site, "cooperative helper expansion exceeds its proof depth budget");
    Frame frame;
    for (std::size_t i = 0; i < arguments.size(); ++i)
      frame.symbols[function.parameters[i]] = std::move(arguments[i]);
    const auto previous = context;
    context += "/" + std::to_string(site) + "@" + std::to_string(id);
    suite(frame, function.body);
    auto value = evaluate(frame, function.result);
    if (frame.failure) {
      value = unknown(function.resultType, frame.failure->uniform);
      value.errors.push_back(*frame.failure);
    }
    if (frame.transfer && !frame.failure)
      reject(site, "cooperative helper has an unresolved early exit");
    context = previous;
    --depth;
    return value;
  }
  Value workgroup(Frame &frame, ValueId id, const WorkgroupOperation &operation) {
    using Action = WorkgroupAction;
    const auto action = operation.metadata.action;
    const auto arity = action == Action::Barrier ? 1u : action == Action::Write ? 3u : 2u;
    if (action == Action::Dispatch)
      reject(id, "a cooperative callback cannot launch another dispatch");
    if (operation.arguments.size() != arity)
      reject(id, "invalid cooperative intrinsic argument layout");
    const auto receiver = evaluate(frame, operation.arguments[0]);
    if (action == Action::Read || action == Action::Write) {
      if (receiver.kind != Value::Kind::Scratch || receiver.identity >= result.scratch.size())
        reject(id, "shared scratch provenance is not established");
      auto index = evaluate(frame, operation.arguments[1]);
      if (!index.integer || *index.integer < 0 || *index.integer > UINT64_MAX)
        reject(id, "cannot prove shared scratch bounds and lane ownership for this index");
      if (action == Action::Write)
        evaluate(frame, operation.arguments[2]);
      if (frame.transfer)
        return unknown(module.expressions[id].type, false);
      lane.phases.back().accesses.push_back({receiver.identity,
          static_cast<std::uint64_t>(*index.integer), action == Action::Write, true, id});
      frame.cache.clear();
      return unknown(module.expressions[id].type, receiver.uniform && index.uniform);
    }
    if (receiver.kind != Value::Kind::Item)
      reject(id, "WorkItem provenance is not established");
    if (action == Action::Shared) {
      const auto identity = key(id, false);
      auto found = scratchIds.find(identity);
      if (found == scratchIds.end()) {
        found = scratchIds.emplace(identity, result.scratch.size()).first;
        result.scratch.push_back({operation.metadata.element, operation.metadata.count});
        result.scratchSites.push_back(id);
      }
      allocations.push_back(found->second);
      Value value;
      value.kind = Value::Kind::Scratch;
      value.identity = found->second;
      value.uniform = true;
      return value;
    }
    if (action == Action::Barrier) {
      const auto identity = key(id, true);
      auto found = barrierIds.find(identity);
      if (found == barrierIds.end()) {
        found = barrierIds.emplace(identity, result.barrierSites.size()).first;
        result.barrierSites.push_back(id);
      }
      lane.phases.back().barrier = found->second;
      lane.phases.emplace_back();
      frame.cache.clear();
      return unknown(module.expressions[id].type, true);
    }
    auto axis = evaluate(frame, operation.arguments[1]);
    if (!axis.integer || *axis.integer < 0 || *axis.integer >= geometry.rank)
      reject(id, "workgroup index axis must be constant and inside the logical launch rank");
    const auto dimension = static_cast<std::size_t>(*axis.integer);
    if (action == Action::LocalIndex) {
      const auto coordinates = workgroupCoordinates(geometry, 0, laneId);
      return integer(coordinates->local[dimension], geometry.groupShape[dimension] == 1);
    }
    if (action != Action::GlobalIndex)
      reject(id, "invalid cooperative intrinsic operation");
    return unknown(module.expressions[id].type, geometry.groupShape[dimension] == 1);
  }
  Value binary(Frame &frame, ValueId id, const ScalarBinary &operation) {
    const auto a = evaluate(frame, operation.left), b = evaluate(frame, operation.right);
    auto value = unknown(module.expressions[id].type, a.uniform && b.uniform);
    if (!a.integer || !b.integer)
      return value;
    __int128_t number = 0;
    bool invalid = false;
    switch (operation.operation) {
    case TokenKind::Plus: invalid = __builtin_add_overflow(*a.integer, *b.integer, &number); break;
    case TokenKind::Minus: invalid = __builtin_sub_overflow(*a.integer, *b.integer, &number); break;
    case TokenKind::Star: invalid = __builtin_mul_overflow(*a.integer, *b.integer, &number); break;
    case TokenKind::Slash:
    case TokenKind::Percent:
      if (!*b.integer)
        return value;
      number = operation.operation == TokenKind::Slash ? *a.integer / *b.integer
                                                       : *a.integer % *b.integer;
      break;
    default: return value;
    }
    const auto type = module.expressions[id].type.element;
    if (!invalid && isInteger(type) && number >= integerMinimum(type) && number <= integerMaximum(type))
      value.integer = number;
    return value;
  }
  Value evaluate(Frame &frame, ValueId id) {
    step(id);
    if (id >= module.expressions.size())
      reject(id, "invalid cooperative expression identity");
    if (frame.transfer || frame.failure)
      return unknown(module.expressions[id].type, false);
    if (auto found = frame.cache.find(id); found != frame.cache.end())
      return found->second;
    const auto &node = module.expressions[id];
    auto value = unknown(node.type, false);
    if (const auto *read = std::get_if<SymbolRead>(&node.operation)) {
      auto found = frame.symbols.find(read->symbol);
      if (found == frame.symbols.end())
        reject(id, "cooperative local value has no proven definition");
      value = found->second;
    } else if (const auto *op = std::get_if<WorkgroupOperation>(&node.operation))
      value = workgroup(frame, id, *op);
    else if (const auto *op = std::get_if<ScalarBinary>(&node.operation))
      value = binary(frame, id, *op);
    else if (const auto *op = std::get_if<ScalarCompare>(&node.operation)) {
      const auto a = evaluate(frame, op->left), b = evaluate(frame, op->right);
      value.uniform = a.uniform && b.uniform;
      if (a.integer && b.integer) {
        switch (op->operation) {
        case ComparisonKind::Equal: value.integer = *a.integer == *b.integer; break;
        case ComparisonKind::NotEqual: value.integer = *a.integer != *b.integer; break;
        case ComparisonKind::Less: value.integer = *a.integer < *b.integer; break;
        case ComparisonKind::LessEqual: value.integer = *a.integer <= *b.integer; break;
        case ComparisonKind::Greater: value.integer = *a.integer > *b.integer; break;
        case ComparisonKind::GreaterEqual: value.integer = *a.integer >= *b.integer; break;
        }
      }
    } else if (const auto *op = std::get_if<ScalarUnary>(&node.operation)) {
      value = evaluate(frame, op->operand);
      if (value.integer) {
        if (op->operation == TokenKind::Bang)
          value.integer = !*value.integer;
        else if (op->operation == TokenKind::Minus)
          value.integer = -*value.integer;
        else if (op->operation != TokenKind::Plus)
          value.integer.reset();
      }
    } else if (const auto *op = std::get_if<NumericConvert>(&node.operation)) {
      value = evaluate(frame, op->input);
      if (!isInteger(node.type.element) || (value.integer &&
          (*value.integer < integerMinimum(node.type.element) ||
           *value.integer > integerMaximum(node.type.element))))
        value.integer.reset();
    } else if (const auto *op = std::get_if<ConditionalOperation>(&node.operation)) {
      const auto condition = evaluate(frame, op->condition);
      if (frame.transfer || frame.failure)
        return value;
      const bool taken = choose(id, condition);
      const auto previous = frame.uniformControl;
      frame.uniformControl &= condition.uniform;
      suite(frame, taken ? op->whenTrue : op->whenFalse);
      if (!frame.transfer && !frame.failure)
        value = evaluate(frame, taken ? op->trueResult : op->falseResult);
      value.uniform &= condition.uniform;
      frame.uniformControl = previous;
    } else if (const auto *op = std::get_if<FunctionCall>(&node.operation)) {
      std::vector<Value> arguments;
      for (auto input : op->arguments)
        arguments.push_back(evaluate(frame, input));
      if (!frame.transfer && !frame.failure)
        value = invoke(op->function, std::move(arguments), id);
      if (mutatesBacking(op->function))
        invalidateBackings(frame);
    } else if (const auto *op = std::get_if<FunctionReference>(&node.operation)) {
      value.kind = Value::Kind::Function;
      value.identity = op->function;
      value.uniform = true;
      value.mutableBacking = false;
      for (auto input : op->captures) {
        value.elements.push_back(evaluate(frame, input));
        value.uniform &= value.elements.back().uniform;
        value.mutableBacking |= value.elements.back().mutableBacking;
      }
    } else if (const auto *op = std::get_if<FunctionInvoke>(&node.operation)) {
      const auto function = evaluate(frame, op->callee);
      if (function.kind != Value::Kind::Function || function.identity >= module.functions.size())
        reject(id, "cooperative helper invocation requires established function provenance");
      std::vector<Value> arguments;
      for (auto input : op->arguments)
        arguments.push_back(evaluate(frame, input));
      arguments.insert(arguments.end(), function.elements.begin(), function.elements.end());
      if (!frame.transfer && !frame.failure)
        value = invoke(function.identity, std::move(arguments), id);
      if (mutatesBacking(function.identity))
        invalidateBackings(frame);
    } else if (const auto *op = std::get_if<WhileOperation>(&node.operation)) {
      suite(frame, op->initial);
      if (frame.transfer || frame.failure)
        return value;
      bool completed = false;
      for (std::size_t turn = 0; turn < limits.loopIterations; ++turn) {
        frame.cache.clear();
        const auto condition = evaluate(frame, op->condition);
        if (!condition.integer)
          reject(id, "cooperative loop requires a finite control proof; move independent numerical work into a helper");
        if (!*condition.integer) {
          const auto previous = frame.uniformControl;
          frame.uniformControl &= condition.uniform;
          suite(frame, op->whenDone);
          if (op->normalResult)
            value = evaluate(frame, *op->normalResult);
          value.uniform &= condition.uniform;
          frame.uniformControl = previous;
          completed = true;
          break;
        }
        const auto oldIteration = iteration;
        const auto oldControl = frame.uniformControl;
        frame.uniformControl &= condition.uniform;
        iteration += ":loop" + std::to_string(id) + ":" + std::to_string(turn);
        // For-loop iteration prefixes bind the current item and advance their
        // private cursor BEFORE the source body, so continue cannot repeat it.
        suite(frame, op->iteration);
        suite(frame, op->body);
        iteration = oldIteration;
        if (frame.failure) {
          frame.uniformControl = oldControl;
          return value;
        }
        if (frame.transfer) {
          if (frame.transfer->loop != op->identity) {
            frame.uniformControl = oldControl;
            return value;
          }
          const bool continuing = frame.transfer->continuing;
          value = frame.transfer->value;
          frame.transfer.reset();
          if (!continuing) {
            frame.uniformControl = oldControl;
            completed = true;
            break;
          }
        }
        frame.uniformControl = oldControl;
      }
      if (!completed)
        reject(id, "cooperative loop expansion exceeds its proof iteration budget");
    } else if (const auto *op = std::get_if<LoopTransferOperation>(&node.operation)) {
      if (op->value)
        value = evaluate(frame, *op->value);
      value.uniform &= frame.uniformControl;
      frame.transfer = ControlExit{op->loop, op->continuing, value};
    } else if (const auto *op = std::get_if<TensorLiteral>(&node.operation)) {
      value.kind = Value::Kind::Collection;
      value.uniform = true;
      for (auto input : op->elements) {
        value.elements.push_back(evaluate(frame, input));
        value.uniform &= value.elements.back().uniform;
      }
    } else if (const auto *op = std::get_if<ShapeCheck>(&node.operation))
      value = evaluate(frame, op->input);
    else if (const auto *op = std::get_if<CollectionQuery>(&node.operation)) {
      const auto input = evaluate(frame, op->input);
      const auto &type = module.expressions[op->input].type;
      if (op->kind == CollectionQueryKind::Shape) {
        value.kind = Value::Kind::Collection;
        value.uniform = true;
        for (const auto &extent : type.shape) {
          auto dimension = unknown(ValueType{}, input.uniform);
          if (extent.constant)
            dimension = integer(*extent.constant);
          value.uniform &= dimension.uniform;
          value.elements.push_back(std::move(dimension));
        }
      } else {
        value.uniform = input.uniform;
        if (!type.shape.empty() && type.shape.front().constant)
          value = integer(*type.shape.front().constant);
      }
    } else if (const auto *op = std::get_if<TensorIndex>(&node.operation)) {
      value.uniform = evaluate(frame, op->input).uniform;
      for (auto index : op->indices)
        value.uniform &= evaluate(frame, index).uniform;
    } else if (const auto *op = std::get_if<ArrayBoundsCheck>(&node.operation)) {
      evaluate(frame, op->input);
      value = evaluate(frame, op->index);
    } else if (const auto *op = std::get_if<ArrayStore>(&node.operation)) {
      evaluate(frame, op->input);
      evaluate(frame, op->index);
      evaluate(frame, op->value);
      invalidateBackings(frame);
    } else if (const auto *op = std::get_if<FailOperation>(&node.operation)) {
      auto error = evaluate(frame, op->error);
      error.uniform &= frame.uniformControl;
      frame.failure = std::move(error);
    } else if (const auto *op = std::get_if<TaskOperation>(&node.operation);
               op && op->prefix == TaskPrefix::Try && !node.type.task) {
      value = evaluate(frame, op->operand);
      if (!value.errors.empty()) {
        frame.failure = value.errors.front();
        frame.failure->uniform &= frame.uniformControl && value.uniform;
      }
      value.errors.clear();
    } else if (const auto *op = std::get_if<CatchOperation>(&node.operation)) {
      value = evaluate(frame, op->operand);
      if (!value.errors.empty() && !frame.failure) {
        const auto previous = frame.uniformControl;
        frame.uniformControl &= value.errors.front().uniform && value.uniform;
        frame.symbols[op->parameter] = value.errors.front();
        suite(frame, op->body);
        value = evaluate(frame, op->result);
        value.uniform &= frame.uniformControl;
        frame.uniformControl = previous;
      }
    } else if (const auto *op = std::get_if<GuardOperation>(&node.operation)) {
      const auto condition = evaluate(frame, op->condition);
      if (!frame.failure && !frame.transfer && choose(id, condition)) {
        const auto previous = frame.uniformControl;
        frame.uniformControl &= condition.uniform;
        suite(frame, op->body);
        frame.uniformControl = previous;
      }
    } else if (const auto *op = std::get_if<RecordValue>(&node.operation)) {
      if (op->base)
        value = evaluate(frame, *op->base);
      else {
        value.uniform = true;
        value.elements.resize(module.records[*node.type.record].fields.size());
      }
      if (op->variant && !value.elements.empty())
        value.elements.front() = integer(*op->variant + 1);
      for (const auto &field : op->fields) {
        auto member = evaluate(frame, field.value);
        if (field.field >= value.elements.size())
          reject(id, "cooperative record field has no established layout");
        value.elements[field.field] = member;
        value.uniform &= member.uniform;
      }
    } else if (const auto *op = std::get_if<RecordFieldRead>(&node.operation)) {
      const auto record = evaluate(frame, op->input);
      value.uniform = record.uniform;
      if (op->field < record.elements.size())
        value = record.elements[op->field];
      value.uniform &= record.uniform;
    } else if (const auto *op = std::get_if<MatchOperation>(&node.operation)) {
      const auto subject = evaluate(frame, op->subject);
      if (subject.elements.empty() || !subject.elements.front().integer)
        reject(id, "cooperative variant matching requires an established private enum tag");
      bool matched = false;
      for (const auto &arm : op->cases)
        if (!arm.tag || *arm.tag == *subject.elements.front().integer) {
          matched = true;
          const auto previous = frame.uniformControl;
          frame.uniformControl &= subject.uniform;
          suite(frame, arm.body);
          value = evaluate(frame, arm.result);
          value.uniform &= frame.uniformControl;
          frame.uniformControl = previous;
          break;
        }
      if (!matched)
        reject(id, "cooperative variant match has no established arm");
    } else if (const auto *op = std::get_if<IntegerRangeOperation>(&node.operation)) {
      const auto current = evaluate(frame, op->current), stop = evaluate(frame, op->stop),
                 stride = evaluate(frame, op->step);
      value.uniform = current.uniform && stop.uniform && stride.uniform;
      if (current.integer && stop.integer && stride.integer && *stride.integer) {
        if (op->kind == IntegerRangeOperation::Kind::Test)
          value.integer = *stride.integer > 0 ? *current.integer < *stop.integer
                                             : *current.integer > *stop.integer;
        else if (op->kind == IntegerRangeOperation::Kind::Advance) {
          const auto next = *current.integer + *stride.integer;
          value.integer = next < INT64_MIN || next > INT64_MAX ? *stop.integer : next;
        } else
          value.integer = *current.integer;
      }
    } else if (const auto *op = std::get_if<ArrayIndex>(&node.operation)) {
      const auto array = evaluate(frame, op->input), index = evaluate(frame, op->index);
      value.uniform = array.uniform && index.uniform;
      if (index.integer && *index.integer >= 0 &&
          static_cast<__uint128_t>(*index.integer) < array.elements.size())
        value = array.elements[static_cast<std::size_t>(*index.integer)];
      value.uniform &= array.uniform && index.uniform;
    } else {
      bool literal = true;
      if (const auto *n = std::get_if<Int32Literal>(&node.operation)) value = integer(n->value);
      else if (const auto *n = std::get_if<Int64Literal>(&node.operation)) value = integer(n->value);
      else if (const auto *n = std::get_if<UInt32Literal>(&node.operation)) value = integer(n->value);
      else if (const auto *n = std::get_if<UInt64Literal>(&node.operation)) value = integer(n->value);
      else if (const auto *n = std::get_if<BoolLiteral>(&node.operation)) value = integer(n->value);
      else if (std::holds_alternative<Float32Literal>(node.operation) ||
               std::holds_alternative<Float64Literal>(node.operation)) value.uniform = true;
      else literal = false;
      if (!literal)
        reject(id, "this cooperative expression does not yet have an established control/effect proof");
    }
    frame.cache[id] = value;
    return value;
  }
public:
  Analyzer(const TypedModule &input, ValueId id, const WorkgroupAnalysisLimits &options)
      : module(input), limits(options),
        dispatch(std::get<WorkgroupOperation>(input.expressions.at(id).operation)),
        declaration(input.expressions.at(id).span) {
    if (dispatch.metadata.action != WorkgroupAction::Dispatch || dispatch.arguments.size() != 3)
      reject(id, "expected a checked cooperative dispatch");
    const std::vector<std::int64_t> shape(dispatch.groupShape.begin(), dispatch.groupShape.end());
    const auto domain = makeWorkgroupGeometry(shape, dispatch.groupShape,
        static_cast<std::uint32_t>(std::min<std::size_t>(limits.proof.lanes, UINT32_MAX)),
        UINT64_MAX, 1);
    if (!domain)
      reject(id, "cooperative group shape exceeds its geometry or proof budget");
    geometry = *domain;
    effects.resize(module.functions.size());
    for (std::size_t fn = 0; fn < module.functions.size(); ++fn)
      for (auto expression : module.functions[fn].expressions) {
        const auto &operation = module.expressions[expression].operation;
        effects[fn] = effects[fn] || std::holds_alternative<WorkgroupOperation>(operation) ||
                      std::holds_alternative<FunctionInvoke>(operation) ||
                      std::holds_alternative<ConsoleOperation>(operation) ||
                      std::holds_alternative<FormatOperation>(operation) ||
                      std::holds_alternative<FileOperation>(operation) ||
                      std::holds_alternative<BufferOperation>(operation) ||
                      std::holds_alternative<IteratorOperation>(operation) ||
                      std::holds_alternative<TimeOperation>(operation) ||
                      std::holds_alternative<EnvironmentLookup>(operation) ||
                      std::holds_alternative<TaskOperation>(operation) ||
                      std::holds_alternative<FailOperation>(operation) ||
                      std::holds_alternative<CatchOperation>(operation) ||
                      std::holds_alternative<Transfer>(operation) ||
                      std::holds_alternative<ParallelOperation>(operation) ||
                      std::holds_alternative<YieldOperation>(operation);
      }
    for (std::size_t turn = 0; turn < module.functions.size(); ++turn) {
      bool changed = false;
      for (std::size_t fn = 0; fn < module.functions.size(); ++fn)
        if (!effects[fn])
          for (auto expression : module.functions[fn].expressions) {
            const bool effect = std::visit([&](const auto &operation) {
              if constexpr (requires { operation.function; }) {
                if constexpr (std::is_integral_v<std::remove_cvref_t<decltype(operation.function)>>)
                  return operation.function < effects.size() && effects[operation.function];
              }
              return false;
            }, module.expressions[expression].operation);
            if (effect) {
              effects[fn] = true;
              changed = true;
              break;
            }
          }
      if (!changed)
        break;
    }
  }
  WorkgroupAnalysis run(ValueId id) {
    const auto &target = module.expressions[dispatch.arguments[2]];
    const auto *reference = std::get_if<FunctionReference>(&target.operation);
    if (!reference || reference->function >= module.functions.size())
      reject(id, "cooperative dispatch requires established callback provenance");
    const auto &function = module.functions[reference->function];
    std::vector<std::map<std::string, bool>> scenarios(1);
    while (!scenarios.empty()) {
      uniformChoices = std::move(scenarios.back());
      scenarios.pop_back();
      if (++result.uniformPaths > limits.uniformPaths)
        reject(id, "cooperative proof exceeds its group-uniform path budget");
      std::vector<WorkgroupLaneAlternatives> lanes(geometry.localSize);
      std::optional<std::vector<std::size_t>> collectiveAllocations;
      bool retry = false;
      for (laneId = 0; laneId < geometry.localSize && !retry; ++laneId) {
        std::vector<std::map<std::string, bool>> paths(1);
        std::size_t explored = 0;
        while (!paths.empty()) {
          if (++explored > 2 * limits.proof.pathsPerLane)
            reject(id, "cooperative proof exceeds its invocation path budget");
          localChoices = std::move(paths.back());
          paths.pop_back();
          lane = {};
          lane.phases.emplace_back();
          allocations.clear();
          context.clear();
          iteration.clear();
          depth = 0;
          std::vector<Value> arguments;
          for (auto parameter : function.parameters) {
            const auto &type = module.symbols[parameter].type;
            arguments.push_back(unknown(type, !workgroupCapability(module, type)));
          }
          try {
            invoke(reference->function, std::move(arguments), id, true);
          } catch (const Choice &choice) {
            auto base = choice.uniform ? uniformChoices : localChoices;
            base[choice.identity] = false;
            (choice.uniform ? scenarios : paths).push_back(base);
            base[choice.identity] = true;
            (choice.uniform ? scenarios : paths).push_back(std::move(base));
            if (choice.uniform) {
              retry = true;
              break;
            }
            continue;
          }
          if (collectiveAllocations && allocations != *collectiveAllocations)
            reject(id, "shared declarations must be reached collectively by the workgroup");
          collectiveAllocations = allocations;
          lanes[laneId].paths.push_back(lane);
        }
      }
      if (retry)
        continue;
      const auto proof = proveWorkgroupAlternatives(result.scratch, lanes, limits.proof);
      if (!proof) {
        const auto &error = proof.error();
        const auto &trace = lanes[error.lane].paths[error.path].phases;
        auto source = id;
        if (error.phase < trace.size()) {
          if (error.access < trace[error.phase].accesses.size())
            source = trace[error.phase].accesses[error.access].source;
          else if (trace[error.phase].barrier)
            source = result.barrierSites[*trace[error.phase].barrier];
        }
        reject(source, error.message);
      }
      result.scratchBytes = proof->scratchBytes;
    }
    return result;
  }
};
} // namespace
std::expected<WorkgroupAnalysis, Diagnostic>
analyzeWorkgroup(const TypedModule &module, ValueId dispatch, const WorkgroupAnalysisLimits &limits) {
  try {
    return Analyzer(module, dispatch, limits).run(dispatch);
  } catch (const Rejected &error) {
    return std::unexpected(error.diagnostic);
  }
}
} // namespace kalandra
