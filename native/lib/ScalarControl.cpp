#include "ScalarControl.h"
#include "NonCompleting.h"
#include "TypedChildren.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
namespace kalandra {
namespace {
struct UnsupportedControl {};
bool scalar(const ValueType &t) {
  return !t.array && !t.tensor && !t.task && !t.fallible && !t.device &&
         (t.builtin == BuiltinValue::Numeric || isBooleanStorage(t));
}
class Control {
  TypedModule &out;
  const TypedModule source;
  FunctionId owner;
  ValueType boolean, integer;
  SymbolId pending;
  std::map<std::size_t, SymbolId> results;
  std::set<std::size_t> loops;
  std::map<ValueId, ValueId> values;
  std::map<ValueId, bool> transfers;
  std::map<SymbolId, SymbolId> aliases;
  std::set<SymbolId> cells;
  std::vector<ValueId> emitted;
  std::vector<bool> nonCompleting;
  std::size_t limit;
  bool sameScalar(const ValueType &a, const ValueType &b) {
    return scalar(a) && scalar(b) && a.builtin == b.builtin &&
           (a.builtin != BuiltinValue::Numeric || a.element == b.element);
  }
  void require(bool ok) {
    if (!ok)
      throw UnsupportedControl{};
  }
  template <class Op> ValueId add(ValueType type, SourceSpan span, Op op) {
    require(out.expressions.size() < limit);
    const auto id = out.expressions.size();
    out.expressions.push_back({span, std::move(type), std::move(op)});
    emitted.push_back(id);
    return id;
  }
  SymbolId cell(ValueType type, SourceSpan span) {
    const auto id = out.symbols.size();
    out.symbols.push_back({"__loop_control_" + std::to_string(id), type, span, owner, false});
    out.symbols.back().reassignable = true;
    return id;
  }
  ValueId read(SymbolId id, SourceSpan span) {
    while (aliases.contains(id))
      id = aliases.at(id);
    const auto type = out.symbols[id].type;
    if (type.builtin == BuiltinValue::Unit)
      return add(type, span, BoolLiteral{false});
    return add(type, span, SymbolRead{id});
  }
  ValueId zero(ValueType type, SourceSpan span) {
    if (isBooleanStorage(type))
      return add(type, span, BoolLiteral{false});
    if (type.element == NumericType::Float32)
      return add(type, span, Float32Literal{0});
    if (type.element == NumericType::Float64)
      return add(type, span, Float64Literal{0});
    if (isUnsigned(type.element)) {
      if (describeNumeric(type.element)->bits <= 32)
        return add(type, span, UInt32Literal{0});
      return add(type, span, UInt64Literal{0});
    }
    if (describeNumeric(type.element)->bits <= 32)
      return add(type, span, Int32Literal{0});
    return add(type, span, Int64Literal{0});
  }
  ValueId code(std::int64_t value, SourceSpan span) {
    return add(integer, span, Int64Literal{value});
  }
  ValueId test(std::int64_t value, SourceSpan span) {
    return add(boolean, span,
               ScalarCompare{ComparisonKind::Equal, read(pending, span), code(value, span)});
  }
  ValueId block(std::vector<TypedStatement> body, SourceSpan span) {
    auto no = add(boolean, span, BoolLiteral{false});
    body.push_back({span, {}, no});
    return add(
        boolean, span,
        ConditionalOperation{
            add(boolean, span, BoolLiteral{true}), std::move(body), {{span, {}, no}}, no, no});
  }
  ValueId guarded(ValueId condition, std::vector<TypedStatement> body, SourceSpan span) {
    const auto no = add(boolean, span, BoolLiteral{false});
    body.push_back({span, {}, no});
    return add(boolean, span,
               ConditionalOperation{condition, std::move(body), {{span, {}, no}}, no, no});
  }
  bool mayTransfer(ValueId id) {
    if (auto found = transfers.find(id); found != transfers.end())
      return found->second;
    const auto &node = source.expressions[id];
    bool answer = std::holds_alternative<LoopTransferOperation>(node.operation);
    visitTypedChildren(
        node, [&](ValueId child) { answer = mayTransfer(child) || answer; }, [](SymbolId) {});
    return transfers[id] = answer;
  }
  void admit(ValueId id, bool nested, std::set<ValueId> &seen) {
    if (!seen.insert(id).second)
      return;
    const auto &node = source.expressions[id];
    const auto body = [&](const auto &statements, bool inside) {
      for (const auto &s : statements) {
        if (s.binding && !scalar(source.symbols[*s.binding].type) && inside)
          require(aliases.contains(*s.binding));
        admit(s.value, inside, seen);
      }
    };
    if (const auto *loop = std::get_if<WhileOperation>(&node.operation)) {
      require(scalar(node.type) && loop->identity &&
              loop->identity < std::size_t(std::numeric_limits<std::int64_t>::max()) &&
              !loop->collecting && !loop->traversalErrorBoundary);
      loops.insert(loop->identity);
      if (node.type.builtin != BuiltinValue::Unit)
        results[loop->identity] = cell(node.type, node.span);
      body(loop->initial, nested);
      admit(loop->condition, true, seen);
      body(loop->iteration, true);
      body(loop->body, true);
      body(loop->whenDone, nested);
      if (loop->normalResult)
        admit(*loop->normalResult, nested, seen);
      return;
    }
    if (const auto *choice = std::get_if<ConditionalOperation>(&node.operation)) {
      admit(choice->condition, nested, seen);
      body(choice->whenTrue, nested);
      body(choice->whenFalse, nested);
      admit(choice->trueResult, nested, seen);
      admit(choice->falseResult, nested, seen);
      return;
    }
    visitTypedChildren(
        node,
        [&](ValueId child) {
          admit(child, nested || std::holds_alternative<WhileOperation>(node.operation), seen);
        },
        [&](SymbolId symbol) {
          const auto &t = source.symbols[symbol].type;
          if (!scalar(t) && nested) {
            // Canonical for loops bind a read-only source alias in their initial
            // suite. Eliminate that descriptor alias, not its backing owner.
            require(aliases.contains(symbol));
          }
        });
  }
  std::vector<TypedStatement> suite(const std::vector<TypedStatement> &body) {
    std::vector<TypedStatement> result;
    for (std::size_t n = 0; n < body.size(); ++n) {
      auto statement = body[n];
      statement.value = expression(statement.value);
      if (statement.binding) {
        const auto symbol = *statement.binding;
        if (aliases.contains(symbol) || out.symbols[symbol].type.builtin == BuiltinValue::Unit)
          statement.binding.reset();
      }
      if (statement.binding && mayTransfer(body[n].value)) {
        // A transfer in an assignment's RHS must not publish a placeholder
        // value to a source binding (for example total=if ...: break else:...).
        const auto type = source.expressions[body[n].value].type;
        require(scalar(type));
        if (!sameScalar(type, out.symbols[*statement.binding].type)) {
          require(nonCompleting[body[n].value]);
          statement.binding.reset();
          result.push_back(statement);
          if (n + 1 < body.size()) {
            std::vector<TypedStatement> rest(body.begin() + n + 1, body.end());
            result.push_back({statement.span,
                              {},
                              guarded(test(0, statement.span), suite(rest), statement.span)});
          }
          break;
        }
        const auto temporary = cell(type, statement.span);
        result.push_back({statement.span, temporary, statement.value});
        result.push_back(
            {statement.span,
             {},
             guarded(test(0, statement.span),
                     {{statement.span, statement.binding, read(temporary, statement.span)}},
                     statement.span)});
      } else
        result.push_back(statement);
      if (mayTransfer(body[n].value) && n + 1 < body.size()) {
        std::vector<TypedStatement> rest(body.begin() + n + 1, body.end());
        result.push_back(
            {statement.span, {}, guarded(test(0, statement.span), suite(rest), statement.span)});
        break;
      }
    }
    return result;
  }
  std::vector<TypedStatement> withResult(std::vector<TypedStatement> body, ValueId value,
                                         SymbolId target, SourceSpan span) {
    const auto type = out.symbols[target].type;
    if (!sameScalar(source.expressions[value].type, type)) {
      require(nonCompleting[value]);
      if (body.empty() || body.back().value != value)
        body.push_back({span, {}, value});
      return suite(body);
    }
    const auto temporary = cell(type, span);
    if (!body.empty() && body.back().value == value && !body.back().binding)
      body.back().binding = temporary;
    else
      body.push_back({span, temporary, value});
    auto result = suite(body);
    result.insert(result.begin(), {span, temporary, zero(type, span)});
    result.push_back(
        {span, {}, guarded(test(0, span), {{span, target, read(temporary, span)}}, span)});
    return result;
  }
  ValueId expression(ValueId id) {
    if (auto found = values.find(id); found != values.end())
      return found->second;
    const auto &original = source.expressions[id];
    if (const auto *r = std::get_if<SymbolRead>(&original.operation))
      return read(r->symbol, original.span);
    if (const auto *jump = std::get_if<LoopTransferOperation>(&original.operation)) {
      require(loops.contains(jump->loop));
      std::vector<TypedStatement> body;
      if (jump->value && results.contains(jump->loop))
        body = withResult({}, *jump->value, results.at(jump->loop), original.span);
      else if (jump->value)
        body.push_back({original.span, {}, expression(*jump->value)});
      body.push_back(
          {original.span,
           {},
           guarded(test(0, original.span),
                   {{original.span, pending,
                     code(jump->continuing ? -std::int64_t(jump->loop) : std::int64_t(jump->loop),
                          original.span)}},
                   original.span)});
      return values[id] = block(std::move(body), original.span);
    }
    if (const auto *loop = std::get_if<WhileOperation>(&original.operation)) {
      const auto span = original.span;
      auto initial = suite(loop->initial);
      if (results.contains(loop->identity))
        initial.insert(initial.begin(),
                       {span, results.at(loop->identity), zero(original.type, span)});
      const auto condition =
          add(boolean, span,
              ConditionalOperation{test(0, span),
                                   {{span, {}, expression(loop->condition)}},
                                   {{span, {}, add(boolean, span, BoolLiteral{false})}},
                                   expression(loop->condition),
                                   add(boolean, span, BoolLiteral{false})});
      auto body = suite(loop->body);
      body.push_back({span,
                      {},
                      guarded(test(-std::int64_t(loop->identity), span),
                              {{span, pending, code(0, span)}}, span)});
      const auto iter = add(boolean, span,
                            WhileOperation{condition,
                                           std::move(body),
                                           {},
                                           {},
                                           loop->identity,
                                           false,
                                           std::move(initial),
                                           suite(loop->iteration)});
      auto normal =
          loop->normalResult && results.contains(loop->identity)
              ? withResult(loop->whenDone, *loop->normalResult, results.at(loop->identity), span)
              : suite(loop->whenDone);
      std::vector<TypedStatement> after{{span, {}, iter},
                                        {span, {}, guarded(test(0, span), std::move(normal), span)},
                                        {span,
                                         {},
                                         guarded(test(std::int64_t(loop->identity), span),
                                                 {{span, pending, code(0, span)}}, span)}};
      const auto result = results.contains(loop->identity) ? read(results.at(loop->identity), span)
                                                           : zero(original.type, span);
      after.push_back({span, {}, result});
      const auto unreachable = zero(original.type, span);
      return values[id] = add(original.type, span,
                              ConditionalOperation{add(boolean, span, BoolLiteral{true}),
                                                   std::move(after),
                                                   {{span, {}, unreachable}},
                                                   result,
                                                   unreachable});
    }
    auto node = original;
    if (auto *choice = std::get_if<ConditionalOperation>(&node.operation)) {
      if (mayTransfer(id)) {
        require(scalar(node.type) && node.type.builtin != BuiltinValue::Unit);
        const auto result = cell(node.type, node.span);
        const auto condition = cell(boolean, node.span);
        const auto yes = withResult(choice->whenTrue, choice->trueResult, result, node.span);
        const auto no = withResult(choice->whenFalse, choice->falseResult, result, node.span);
        const auto falseValue = add(boolean, node.span, BoolLiteral{false});
        const auto branch =
            add(boolean, node.span,
                ConditionalOperation{read(condition, node.span), yes, no, falseValue, falseValue});
        std::vector<TypedStatement> body{
            {node.span, result, zero(node.type, node.span)},
            {node.span, condition, expression(choice->condition)},
            {node.span, {}, guarded(test(0, node.span), {{node.span, {}, branch}}, node.span)}};
        const auto returned = read(result, node.span);
        body.push_back({node.span, {}, returned});
        const auto unreachable = zero(node.type, node.span);
        return values[id] = add(node.type, node.span,
                                ConditionalOperation{add(boolean, node.span, BoolLiteral{true}),
                                                     std::move(body),
                                                     {{node.span, {}, unreachable}},
                                                     returned,
                                                     unreachable});
      }
      choice->condition = expression(choice->condition);
      choice->whenTrue = suite(choice->whenTrue);
      choice->whenFalse = suite(choice->whenFalse);
      choice->trueResult = expression(choice->trueResult);
      choice->falseResult = expression(choice->falseResult);
    } else {
      visitTypedChildren(
          node, [&](ValueId &child) { child = expression(child); }, [](SymbolId &) {});
    }
    const auto result = out.expressions.size();
    require(result < limit);
    out.expressions.push_back(std::move(node));
    emitted.push_back(result);
    values[id] = result;
    return result;
  }

public:
  Control(TypedModule &out, FunctionId owner)
      : out(out), source(out), owner(owner), nonCompleting(nonCompletingValues(source)),
        limit(out.expressions.size() +
              std::min<std::size_t>(65536, source.functions[owner].expressions.size() * 12 + 128)) {
    boolean.builtin = BuiltinValue::Bool;
    integer.element = NumericType::Int64;
    pending = cell(integer, source.functions[owner].declaration);
  }
  void run() {
    const auto &fn = source.functions[owner];
    // Scalar storage is private to this invocation. Unit is a canonical bit,
    // not a writable cell. Non-scalar bindings must already dominate the loop.
    for (auto id : fn.expressions) {
      const auto &node = source.expressions[id];
      visitTypedChildren(
          node, [](ValueId) {},
          [&](SymbolId symbol) {
            if (scalar(source.symbols[symbol].type) &&
                source.symbols[symbol].type.builtin != BuiltinValue::Unit)
              cells.insert(symbol);
          });
      if (const auto *loop = std::get_if<WhileOperation>(&node.operation))
        for (const auto &s : loop->initial)
          if (s.binding && !scalar(source.symbols[*s.binding].type))
            if (const auto *read = std::get_if<SymbolRead>(&source.expressions[s.value].operation);
                read && !source.symbols[*s.binding].reassignable &&
                !source.symbols[read->symbol].reassignable)
              aliases[*s.binding] = read->symbol;
    }
    for (auto symbol : fn.parameters)
      if (scalar(source.symbols[symbol].type) &&
          source.symbols[symbol].type.builtin != BuiltinValue::Unit)
        cells.insert(symbol);
    for (const auto &statement : fn.body)
      if (statement.binding && scalar(source.symbols[*statement.binding].type) &&
          source.symbols[*statement.binding].type.builtin != BuiltinValue::Unit)
        cells.insert(*statement.binding);
    std::set<ValueId> seen;
    for (const auto &s : fn.body)
      admit(s.value, false, seen);
    auto body = fn.body;
    require(!body.empty() && !body.back().binding && body.back().value == fn.result);
    body.pop_back();
    std::vector<std::pair<ValueType, std::optional<SymbolId>>> returnedCells;
    std::vector<std::size_t> fields;
    const auto returnValue = [&](ValueType type, ValueId value) {
      require(scalar(type));
      const auto target = type.builtin == BuiltinValue::Unit
                              ? std::optional<SymbolId>{}
                              : std::optional(cell(type, fn.declaration));
      body.push_back({fn.declaration, target, value});
      returnedCells.push_back({type, target});
    };
    if (scalar(fn.resultType))
      returnValue(fn.resultType, fn.result);
    else {
      const auto *record = std::get_if<RecordValue>(&source.expressions[fn.result].operation);
      require(record && !record->base && !record->variant && fn.resultType.record &&
              record->fields.size() == source.records[*fn.resultType.record].fields.size());
      for (const auto &field : record->fields) {
        fields.push_back(field.field);
        returnValue(source.records[*fn.resultType.record].fields[field.field].type, field.value);
      }
    }
    body = suite(body);
    body.insert(body.begin(), {fn.declaration, pending, code(0, fn.declaration)});
    std::vector<ValueId> returnedValues;
    for (const auto &[type, target] : returnedCells) {
      if (target)
        body.insert(body.begin(), {fn.declaration, *target, zero(type, fn.declaration)});
      returnedValues.push_back(target ? read(*target, fn.declaration) : zero(type, fn.declaration));
    }
    ValueId returned;
    if (scalar(fn.resultType))
      returned = returnedValues[0];
    else {
      std::vector<RecordFieldValue> values;
      for (std::size_t n = 0; n < fields.size(); ++n)
        values.push_back({fields[n], returnedValues[n]});
      returned = add(fn.resultType, fn.declaration, RecordValue{{}, std::move(values), {}});
    }
    // Both native and continuation lowering take a function's final statement
    // as its returned value. The guarded tail itself is only a Bool marker.
    body.push_back({fn.declaration, {}, returned});
    for (auto symbol : cells)
      out.symbols[symbol].reassignable = true;
    out.functions[owner].body = std::move(body);
    out.functions[owner].result = returned;
    out.functions[owner].expressions = std::move(emitted);
  }
};
} // namespace
void normalizeFusedScalarControl(TypedModule &module, FunctionId id) {
  if (std::ranges::none_of(module.functions[id].expressions, [&](ValueId value) {
        return std::holds_alternative<LoopTransferOperation>(module.expressions[value].operation);
      }))
    return;
  const auto values = module.expressions.size(), symbols = module.symbols.size();
  try {
    Control(module, id).run();
  } catch (const UnsupportedControl &) {
    module.expressions.resize(values);
    module.symbols.resize(symbols);
  }
}
} // namespace kalandra
