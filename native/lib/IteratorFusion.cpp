#include "IteratorFusion.h"
#include "ScalarControl.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <variant>

namespace kalandra {
namespace {
struct NotFusible {};
struct Producer;
using Binding = std::variant<ValueId, std::shared_ptr<Producer>, FunctionReference>;
struct Environment {
  std::map<SymbolId, Binding> bindings;
  std::map<ValueId, ValueId> values;
  // A consumer continue resumes its producer after the yield, whereas a
  // consumer break abandons the entire producer. They are distinct CFG edges.
  std::map<std::size_t, std::pair<std::size_t, std::size_t>> loops;
  std::function<ValueId(ValueId)> yield;
  bool consumer = false;
};
struct Producer {
  std::optional<FunctionId> function;
  Environment environment;
  std::vector<ValueId> range;
  std::shared_ptr<Producer> limited;
  std::optional<ValueId> limit;
  std::optional<ValueId> array;
};

// Deforestation of scalar, nonescaping producer/consumer graphs. The source
// checker has already established all ownership/loan obligations. This pass
// deliberately does not admit managed items, expected errors, I/O or tasks.
// Those producers retain the general independent-frame implementation.
class Fusion {
  TypedModule &out;
  const TypedModule source;
  FunctionId owner = 0;
  std::size_t identity = 1, work = 0, expansionLimit;
  std::vector<ValueId> emitted;
  std::vector<TypedStatement> captures;
  std::set<FunctionId> resolving;
  ValueType boolean, integer;

  void require(bool condition) const {
    if (!condition)
      throw NotFusible{};
  }
  bool scalar(const ValueType &t) const {
    return !t.array && !t.tensor && !t.task && !t.fallible && !t.device &&
           (t.builtin == BuiltinValue::Numeric || isBooleanStorage(t));
  }
  bool plainRecord(const ValueType &t) const {
    if (t.array || t.tensor || t.task || t.fallible || t.device || !t.record)
      return false;
    const auto &record = source.records[*t.record];
    return !record.error && !record.uninhabited && record.variants.empty() &&
           !record.fields.empty() && std::ranges::all_of(record.fields, [&](const auto &field) {
             return scalar(field.type);
           });
  }
  bool flatArray(const ValueType &t) const {
    return t.array && !t.task && !t.fallible && !t.device &&
           (t.builtin == BuiltinValue::Numeric || isBooleanStorage(t));
  }
  ValueId emit(TypedExpr expression) {
    require(++work < 32000 && out.expressions.size() < expansionLimit);
    const auto id = out.expressions.size();
    out.expressions.push_back(std::move(expression));
    emitted.push_back(id);
    return id;
  }
  template <class Op> ValueId add(ValueType type, SourceSpan span, Op op) {
    return emit({span, std::move(type), std::move(op)});
  }
  ValueId read(SymbolId symbol, SourceSpan span) {
    return add(out.symbols[symbol].type, span, SymbolRead{symbol});
  }
  SymbolId local(ValueType type, SourceSpan span, bool mutableCell = false) {
    const auto id = out.symbols.size();
    out.symbols.push_back({"__fused_" + std::to_string(id), std::move(type), span, owner, false});
    out.symbols.back().reassignable = mutableCell;
    return id;
  }
  ValueId snapshot(ValueId value, SourceSpan span, bool mutableCell = false) {
    const auto symbol = local(out.expressions[value].type, span, mutableCell);
    captures.push_back({span, symbol, value});
    return read(symbol, span);
  }
  Binding binding(SymbolId symbol, Environment &env, SourceSpan span) {
    if (auto found = env.bindings.find(symbol); found != env.bindings.end())
      return found->second;
    require(env.consumer && source.symbols[symbol].owner == owner &&
            (scalar(source.symbols[symbol].type) || plainRecord(source.symbols[symbol].type) ||
             flatArray(source.symbols[symbol].type)));
    return read(symbol, span);
  }
  FunctionReference callable(ValueId id, Environment &env) {
    const auto &node = source.expressions[id];
    if (const auto *symbol = std::get_if<SymbolRead>(&node.operation)) {
      auto bound = binding(symbol->symbol, env, node.span);
      require(std::holds_alternative<FunctionReference>(bound));
      return std::get<FunctionReference>(bound);
    }
    const auto *fn = std::get_if<FunctionReference>(&node.operation);
    require(fn != nullptr);
    auto result = *fn;
    for (auto &capture : result.captures) {
      const auto &type = source.expressions[capture].type;
      if (flatArray(type)) {
        const auto *symbol = std::get_if<SymbolRead>(&source.expressions[capture].operation);
        require(symbol && !source.symbols[symbol->symbol].reassignable);
      }
      require(scalar(type) || plainRecord(type) || flatArray(type));
      capture = snapshot(clone(capture, env), node.span);
    }
    return result;
  }
  Binding capture(ValueId id, Environment &env, bool mutableCell = false) {
    const auto &node = source.expressions[id];
    if (isIterator(source, node.type))
      return producer(id, env);
    if (node.type.builtin == BuiltinValue::Function)
      return callable(id, env);
    if (flatArray(node.type)) {
      // Retain only an already-bound, non-reassigned source. A temporary array
      // whose sole owner is the eliminated cursor needs a separate lifetime
      // proof; extending that allocation's lifetime is not part of fusion.
      const auto *symbol = std::get_if<SymbolRead>(&node.operation);
      require(symbol && !source.symbols[symbol->symbol].reassignable);
    }
    require(scalar(node.type) || plainRecord(node.type) || flatArray(node.type));
    return snapshot(clone(id, env), node.span, mutableCell);
  }
  std::shared_ptr<Producer> producer(ValueId id, Environment &env) {
    const auto &node = source.expressions[id];
    require(isIterator(source, node.type) && !node.type.fallible && !node.type.task);
    if (const auto *symbol = std::get_if<SymbolRead>(&node.operation)) {
      auto bound = binding(symbol->symbol, env, node.span);
      require(std::holds_alternative<std::shared_ptr<Producer>>(bound));
      return std::get<std::shared_ptr<Producer>>(bound);
    }
    if (const auto *call = std::get_if<FunctionCall>(&node.operation)) {
      require(resolving.insert(call->function).second);
      const auto &fn = source.functions[call->function];
      if (fn.standardIteratorTake) {
        require(call->arguments.size() == 2);
        auto result = std::make_shared<Producer>();
        result->limited = producer(call->arguments[0], env);
        result->limit = snapshot(clone(call->arguments[1], env), node.span);
        resolving.erase(call->function);
        return result;
      }
      require(fn.body.size() == 1 && fn.body[0].value == fn.result && !fn.body[0].binding &&
              fn.parameters.size() == call->arguments.size());
      Environment inner;
      for (std::size_t n = 0; n < fn.parameters.size(); ++n)
        inner.bindings[fn.parameters[n]] = capture(call->arguments[n], env);
      auto result = producer(fn.result, inner);
      resolving.erase(call->function);
      return result;
    }
    auto result = std::make_shared<Producer>();
    if (const auto *created = std::get_if<GeneratorCreate>(&node.operation)) {
      const auto &fn = source.functions[created->function];
      require(fn.generator && scalar(fn.generator->item) && !fn.resultType.fallible &&
              fn.parameters.size() == created->captures.size());
      result->function = created->function;
      for (std::size_t n = 0; n < fn.parameters.size(); ++n)
        result->environment.bindings[fn.parameters[n]] =
            capture(created->captures[n], env, source.symbols[fn.parameters[n]].reassignable);
      return result;
    }
    const auto *iterator = std::get_if<IteratorOperation>(&node.operation);
    require(iterator &&
            (iterator->action == IteratorAction::FromRange ||
             iterator->action == IteratorAction::FromArray) &&
            iterator->arguments.size() == 1);
    if (iterator->action == IteratorAction::FromArray) {
      require(flatArray(source.expressions[iterator->arguments[0]].type));
      result->array = snapshot(clone(iterator->arguments[0], env), node.span);
      return result;
    }
    auto value = snapshot(clone(iterator->arguments[0], env), node.span);
    for (std::size_t field = 0; field < 3; ++field)
      result->range.push_back(
          snapshot(add(integer, node.span, RecordFieldRead{value, field}), node.span));
    // Creating a range cursor validates even an empty zero-step source.
    captures.push_back(
        {node.span,
         {},
         add(integer, node.span,
             IntegerRangeOperation{IntegerRangeOperation::Kind::Validate, result->range[0],
                                   result->range[1], result->range[2]})});
    return result;
  }
  std::vector<TypedStatement> suite(const std::vector<TypedStatement> &body, Environment &env) {
    std::vector<TypedStatement> result;
    for (auto statement : body) {
      statement.value = clone(statement.value, env);
      if (statement.binding) {
        const auto old = *statement.binding;
        SymbolId symbol;
        if (auto found = env.bindings.find(old); found != env.bindings.end()) {
          require(std::holds_alternative<ValueId>(found->second));
          const auto *r =
              std::get_if<SymbolRead>(&out.expressions[std::get<ValueId>(found->second)].operation);
          require(r != nullptr);
          symbol = r->symbol;
        } else if (env.consumer && source.symbols[old].owner == owner) {
          symbol = old;
        } else {
          const auto &s = source.symbols[old];
          require(scalar(s.type) || plainRecord(s.type) || flatArray(s.type));
          symbol = local(s.type, s.declaration, s.reassignable);
        }
        statement.binding = symbol;
        env.bindings[old] = read(symbol, statement.span);
      }
      result.push_back(statement);
    }
    return result;
  }
  ValueId expand(const Producer &p, std::function<ValueId(ValueId)> sink, SourceSpan span) {
    if (p.limited) {
      require(p.limit.has_value());
      const auto remaining = local(integer, span, true);
      const auto exit = identity++;
      const auto zero = add(integer, span, Int64Literal{0});
      const auto one = add(integer, span, Int64Literal{1});
      const auto test =
          add(boolean, span, ScalarCompare{ComparisonKind::Greater, read(remaining, span), zero});
      const auto body = expand(
          *p.limited,
          [&](ValueId item) {
            const auto decrement =
                add(integer, span, ScalarBinary{TokenKind::Minus, read(remaining, span), one});
            const auto delivered = sink(item);
            const auto ended = add(
                boolean, span, ScalarCompare{ComparisonKind::Equal, read(remaining, span), zero});
            const auto stop = add(boolean, span, LoopTransferOperation{exit, false, {}});
            const auto no = add(boolean, span, BoolLiteral{false});
            const auto done =
                add(boolean, span,
                    ConditionalOperation{ended, {{span, {}, stop}}, {{span, {}, no}}, stop, no});
            std::vector<TypedStatement> statements{
                {span, remaining, decrement}, {span, {}, delivered}, {span, {}, done}};
            return add(
                boolean, span,
                ConditionalOperation{
                    add(boolean, span, BoolLiteral{true}), statements, {{span, {}, no}}, no, no});
          },
          span);
      return add(
          boolean, span,
          WhileOperation{test,
                         {{span, {}, body},
                          {span, {}, add(boolean, span, LoopTransferOperation{exit, false, {}})}},
                         {},
                         {},
                         exit,
                         true,
                         {{span, remaining, *p.limit}}});
    }
    if (p.function) {
      const auto &fn = source.functions[*p.function];
      require(fn.body.size() == 2 && fn.body.front().value != fn.result);
      auto env = p.environment;
      env.yield = std::move(sink);
      return clone(fn.body.front().value, env);
    }
    if (p.array) {
      const auto current = local(integer, span, true);
      const auto count = local(integer, span);
      const auto itemType = recordArrayElement(out.expressions[*p.array].type);
      const auto item = local(itemType, span);
      const auto index = read(current, span);
      const auto value = add(itemType, span, ArrayIndex{*p.array, index});
      const auto next = add(
          integer, span, ScalarBinary{TokenKind::Plus, index, add(integer, span, Int64Literal{1})});
      const auto body = sink(read(item, span));
      return add(
          boolean, span,
          WhileOperation{
              add(boolean, span,
                  ScalarCompare{ComparisonKind::Less, read(current, span), read(count, span)}),
              {{span, {}, body}},
              {},
              {},
              identity++,
              false,
              {{span, current, add(integer, span, Int64Literal{0})},
               {span, count,
                add(integer, span, CollectionQuery{CollectionQueryKind::Length, *p.array})}},
              {{span, item, value}, {span, current, next}}});
    }
    require(p.range.size() == 3);
    const auto current = local(integer, span, true);
    auto test = add(boolean, span,
                    IntegerRangeOperation{IntegerRangeOperation::Kind::Test, read(current, span),
                                          p.range[1], p.range[2]});
    const auto item = local(integer, span);
    auto advance = add(integer, span,
                       IntegerRangeOperation{IntegerRangeOperation::Kind::Advance,
                                             read(current, span), p.range[1], p.range[2]});
    const auto value = sink(read(item, span));
    WhileOperation loop{test,
                        {{span, {}, value}},
                        {},
                        {},
                        identity++,
                        false,
                        {{span, current, p.range[0]}},
                        {{span, item, read(current, span)}, {span, current, advance}}};
    return add(boolean, span, std::move(loop));
  }
  ValueId loop(ValueId id, WhileOperation op, Environment &env) {
    const auto &node = source.expressions[id];
    // Canonical consuming-for lowering: one owned source, one next/test, one
    // item binding. Manual next/close, loop-valued exits and partial cursor
    // escape deliberately fail this proof rather than changing semantics.
    if (op.initial.size() == 1 && op.initial[0].binding &&
        isIterator(source, source.expressions[op.initial[0].value].type)) {
      require(!op.collecting && !op.traversalErrorBoundary && op.iteration.size() == 1 &&
              op.iteration[0].binding);
      const auto *test = std::get_if<MatchOperation>(&source.expressions[op.condition].operation);
      require(test && test->cases.size() == 2);
      const auto *next =
          std::get_if<IteratorOperation>(&source.expressions[test->subject].operation);
      require(next && next->action == IteratorAction::Next && next->arguments.size() == 1);
      const auto *cursor =
          std::get_if<SymbolRead>(&source.expressions[next->arguments[0]].operation);
      require(cursor && cursor->symbol == *op.initial[0].binding);
      const auto *item =
          std::get_if<RecordFieldRead>(&source.expressions[op.iteration[0].value].operation);
      require(item && item->input == test->subject);
      auto sourceProducer = producer(op.initial[0].value, env);
      const bool plain = !op.transfers && op.whenDone.empty() && !op.normalResult;
      const auto exit = identity++;
      const auto produced = expand(
          *sourceProducer,
          [&, env](ValueId value) mutable {
            auto branch = env;
            const auto continuation = identity++;
            branch.loops[op.identity] = {exit, continuation};
            const auto symbol = local(source.expressions[op.iteration[0].value].type, node.span,
                                      source.symbols[*op.iteration[0].binding].reassignable);
            branch.bindings[*op.iteration[0].binding] = read(symbol, node.span);
            auto body = suite(op.body, branch);
            body.insert(body.begin(), {node.span, symbol, value});
            if (plain) {
              const auto no = add(boolean, node.span, BoolLiteral{false});
              return add(boolean, node.span,
                         ConditionalOperation{add(boolean, node.span, BoolLiteral{true}),
                                              std::move(body),
                                              {{node.span, {}, no}},
                                              no,
                                              no});
            }
            body.push_back(
                {node.span,
                 {},
                 add(boolean, node.span, LoopTransferOperation{continuation, false, {}})});
            return add(boolean, node.span,
                       WhileOperation{add(boolean, node.span, BoolLiteral{true}),
                                      std::move(body),
                                      {},
                                      {},
                                      continuation,
                                      true});
          },
          node.span);
      // A synthetic one-shot break would unnecessarily send ordinary complete
      // traversal through continuation kernels instead of a native subgraph.
      if (plain)
        return produced;
      auto normal = env;
      auto body = suite(op.whenDone, normal);
      const auto completed = op.normalResult ? clone(*op.normalResult, normal)
                                             : add(boolean, node.span, BoolLiteral{false});
      body.insert(body.begin(), {node.span, {}, produced});
      body.push_back(
          {node.span, {}, add(boolean, node.span, LoopTransferOperation{exit, false, completed})});
      std::vector<ValueId> results;
      for (auto value : emitted)
        if (const auto *jump =
                std::get_if<LoopTransferOperation>(&out.expressions[value].operation);
            jump && jump->loop == exit && jump->value)
          results.push_back(*jump->value);
      return add(node.type, node.span,
                 WhileOperation{add(boolean, node.span, BoolLiteral{true}),
                                std::move(body),
                                {},
                                {},
                                exit,
                                true,
                                {},
                                {},
                                std::move(results),
                                false,
                                true});
    }
    require(!op.collecting && !op.functionBoundary && !op.traversalErrorBoundary);
    auto inner = env;
    op.identity = identity++;
    inner.loops[std::get<WhileOperation>(node.operation).identity] = {op.identity, op.identity};
    op.initial = suite(op.initial, inner);
    auto iteration = inner;
    op.condition = clone(op.condition, iteration);
    op.iteration = suite(op.iteration, iteration);
    op.body = suite(op.body, iteration);
    op.whenDone = suite(op.whenDone, inner);
    if (op.normalResult)
      op.normalResult = clone(*op.normalResult, inner);
    for (auto &value : op.exitValues)
      value = clone(value, inner);
    return add(node.type, node.span, std::move(op));
  }
  ValueId inlineScalar(FunctionId target, std::vector<ValueId> arguments, SourceSpan span) {
    const auto &fn = source.functions[target];
    require(resolving.insert(target).second && fn.body.size() == 1 &&
            fn.body[0].value == fn.result && !fn.body[0].binding &&
            fn.parameters.size() == arguments.size());
    Environment inner;
    std::vector<TypedStatement> body;
    for (std::size_t n = 0; n < arguments.size(); ++n) {
      // Even an unused argument can fault. Bind every argument in source order
      // before evaluating the substituted body, rather than substituting a
      // potentially faulting expression at each parameter reference.
      const auto symbol = local(out.expressions[arguments[n]].type, span);
      body.push_back({span, symbol, arguments[n]});
      inner.bindings[fn.parameters[n]] = read(symbol, span);
    }
    const auto value = clone(fn.result, inner);
    body.push_back({span, {}, value});
    resolving.erase(target);
    const auto type = out.expressions[value].type;
    return add(
        type, span,
        ConditionalOperation{add(boolean, span, BoolLiteral{true}), body, body, value, value});
  }
  ValueId clone(ValueId id, Environment &env) {
    if (auto found = env.values.find(id); found != env.values.end())
      return found->second;
    auto node = source.expressions[id];
    require(scalar(node.type) || plainRecord(node.type) || flatArray(node.type));
    if (auto *op = std::get_if<WhileOperation>(&node.operation))
      return loop(id, *op, env);
    if (const auto *op = std::get_if<YieldOperation>(&node.operation)) {
      require(bool(env.yield));
      return env.yield(clone(op->operand, env));
    }
    if (const auto *op = std::get_if<SymbolRead>(&node.operation)) {
      auto bound = binding(op->symbol, env, node.span);
      require(std::holds_alternative<ValueId>(bound));
      const auto value = std::get<ValueId>(bound);
      const auto *r = std::get_if<SymbolRead>(&out.expressions[value].operation);
      return r ? read(r->symbol, node.span) : value;
    }
    if (const auto *op = std::get_if<FunctionCall>(&node.operation)) {
      std::vector<ValueId> args;
      for (auto argument : op->arguments)
        args.push_back(clone(argument, env));
      const auto result = inlineScalar(op->function, std::move(args), node.span);
      env.values[id] = result;
      return result;
    }
    if (const auto *op = std::get_if<FunctionInvoke>(&node.operation)) {
      const auto fn = callable(op->callee, env);
      std::vector<ValueId> args;
      for (auto argument : op->arguments)
        args.push_back(clone(argument, env));
      args.insert(args.end(), fn.captures.begin(), fn.captures.end());
      const auto result = inlineScalar(fn.function, std::move(args), node.span);
      env.values[id] = result;
      return result;
    }
    bool supported = true;
    std::visit(
        [&](auto &op) {
          using T = std::decay_t<decltype(op)>;
          if constexpr (std::is_same_v<T, ScalarBinary> || std::is_same_v<T, ScalarCompare>) {
            op.left = clone(op.left, env);
            op.right = clone(op.right, env);
          } else if constexpr (std::is_same_v<T, ScalarUnary>) {
            op.operand = clone(op.operand, env);
          } else if constexpr (std::is_same_v<T, NumericConvert>) {
            op.input = clone(op.input, env);
          } else if constexpr (std::is_same_v<T, ArrayIndex>) {
            require(flatArray(source.expressions[op.input].type));
            op.input = clone(op.input, env);
            op.index = clone(op.index, env);
          } else if constexpr (std::is_same_v<T, CollectionQuery>) {
            require(op.kind == CollectionQueryKind::Length &&
                    flatArray(source.expressions[op.input].type));
            op.input = clone(op.input, env);
          } else if constexpr (std::is_same_v<T, TensorLiteral>) {
            require(flatArray(node.type));
            for (auto &element : op.elements)
              element = clone(element, env);
          } else if constexpr (std::is_same_v<T, IntegerRangeOperation>) {
            op.current = clone(op.current, env);
            op.stop = clone(op.stop, env);
            op.step = clone(op.step, env);
          } else if constexpr (std::is_same_v<T, RecordFieldRead>) {
            require(plainRecord(source.expressions[op.input].type));
            op.input = clone(op.input, env);
          } else if constexpr (std::is_same_v<T, RecordValue>) {
            require(plainRecord(node.type));
            if (op.base)
              op.base = clone(*op.base, env);
            for (auto &field : op.fields)
              field.value = clone(field.value, env);
          } else if constexpr (std::is_same_v<T, ConditionalOperation>) {
            op.condition = clone(op.condition, env);
            auto left = env, right = env;
            op.whenTrue = suite(op.whenTrue, left);
            op.trueResult = clone(op.trueResult, left);
            op.whenFalse = suite(op.whenFalse, right);
            op.falseResult = clone(op.falseResult, right);
          } else if constexpr (std::is_same_v<T, LoopTransferOperation>) {
            require(env.loops.contains(op.loop));
            const auto [exit, continuation] = env.loops.at(op.loop);
            op.loop = op.continuing ? continuation : exit;
            // A fused consumer continue leaves its one-shot body and resumes
            // immediately after the producer's yield, not at the body's header.
            if (exit != continuation && op.continuing)
              op.continuing = false;
            if (op.value)
              op.value = clone(*op.value, env);
          } else if constexpr (!(std::is_same_v<T, Int32Literal> ||
                                 std::is_same_v<T, Int64Literal> ||
                                 std::is_same_v<T, UInt64Literal> ||
                                 std::is_same_v<T, UInt32Literal> ||
                                 std::is_same_v<T, Float32Literal> ||
                                 std::is_same_v<T, Float64Literal> ||
                                 std::is_same_v<T, BoolLiteral>)) {
            supported = false;
          }
        },
        node.operation);
    require(supported);
    const auto result = emit(std::move(node));
    env.values[id] = result;
    return result;
  }

public:
  explicit Fusion(TypedModule &out)
      : out(out), source(out),
        expansionLimit(source.expressions.size() +
                       std::max<std::size_t>(
                           4096, std::min<std::size_t>(100000, source.expressions.size() * 4))) {
    boolean.builtin = BuiltinValue::Bool;
    integer.element = NumericType::Int64;
    for (const auto &expr : source.expressions)
      if (const auto *loop = std::get_if<WhileOperation>(&expr.operation))
        identity = std::max(identity, loop->identity + 1);
  }
  void run(std::optional<FunctionId> only = {}) {
    for (owner = 0; owner < source.functions.size(); ++owner) {
      const auto &fn = source.functions[owner];
      if ((only && owner != *only) || fn.generator || fn.compileTimeOnly ||
          (!scalar(fn.resultType) && !plainRecord(fn.resultType)) ||
          !std::ranges::any_of(fn.expressions, [&](auto id) {
            return isIterator(source, source.expressions[id].type);
          }))
        continue;
      const auto expressions = out.expressions.size(), symbols = out.symbols.size();
      emitted.clear();
      captures.clear();
      resolving.clear();
      work = 0;
      try {
        // Any cursor read outside one canonical consuming loop/producer edge
        // rejects the entire transaction. This excludes manual partial reads,
        // observable escape and aliasing without guessing at their cleanup.
        std::map<SymbolId, unsigned> reads;
        for (auto id : fn.expressions)
          if (const auto *r = std::get_if<SymbolRead>(&source.expressions[id].operation);
              r && isIterator(source, source.symbols[r->symbol].type))
            ++reads[r->symbol];
        for (auto [symbol, count] : reads) {
          (void)symbol;
          require(count == 1);
        }
        Environment env;
        env.consumer = true;
        std::vector<TypedStatement> body;
        for (auto statement : fn.body) {
          if (statement.binding &&
              source.expressions[statement.value].type.builtin == BuiltinValue::Function) {
            env.bindings[*statement.binding] = callable(statement.value, env);
            body.insert(body.end(), captures.begin(), captures.end());
            captures.clear();
          } else if (statement.binding &&
                     isIterator(source, source.expressions[statement.value].type)) {
            require(reads[*statement.binding] == 1);
            env.bindings[*statement.binding] = producer(statement.value, env);
            body.insert(body.end(), captures.begin(), captures.end());
            captures.clear();
          } else {
            statement.value = clone(statement.value, env);
            require(captures.empty()); // No construction effects may migrate into consumption.
            body.push_back(statement);
          }
        }
        auto result = clone(fn.result, env);
        if (body.empty() || body.back().value != result)
          body.push_back({fn.declaration, {}, result});
        auto &updated = out.functions[owner];
        updated.body = std::move(body);
        updated.result = result;
        updated.expressions = emitted;
        normalizeFusedScalarControl(out, owner);
      } catch (const NotFusible &) {
        out.expressions.resize(expressions);
        out.symbols.resize(symbols);
      }
    }
  }
};
} // namespace
bool fuseLocalIteratorFunction(TypedModule &module, FunctionId id) {
  Fusion(module).run(id);
  return std::ranges::none_of(module.functions[id].expressions, [&](ValueId value) {
    return isIterator(module, module.expressions[value].type);
  });
}
void fuseLocalIterators(TypedModule &module) {
  Fusion(module).run();
  extractLocalIteratorRegions(module);
}
} // namespace kalandra
