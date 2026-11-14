#include "IteratorFusion.h"
#include "TypedChildren.h"
#include "kalandra/Continuation.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace kalandra {
namespace {
bool scalar(const ValueType &type) {
  return !type.array && !type.tensor && !type.task && !type.fallible && !type.device &&
         (type.builtin == BuiltinValue::Numeric || isBooleanStorage(type));
}
struct RegionProof {
  std::set<ValueId> values;
  std::set<SymbolId> reads, writes;
};
RegionProof graph(const TypedModule &module, std::vector<ValueId> pending) {
  RegionProof result;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (!result.values.insert(id).second)
      continue;
    const auto &node = module.expressions.at(id);
    if (const auto *read = std::get_if<SymbolRead>(&node.operation))
      result.reads.insert(read->symbol);
    visitTypedChildren(
        node, [&](ValueId child) { pending.push_back(child); },
        [&](SymbolId symbol) { result.writes.insert(symbol); });
  }
  return result;
}
std::vector<ValueId> roots(const std::vector<TypedStatement> &body) {
  std::vector<ValueId> result;
  for (const auto &statement : body)
    result.push_back(statement.value);
  return result;
}
bool input(const TypedModule &module, const Symbol &symbol) {
  const auto &type = symbol.type;
  if (scalar(type))
    return true;
  if (type.task || type.fallible || type.device)
    return false;
  if (type.array)
    return !symbol.reassignable &&
           (type.builtin == BuiltinValue::Numeric || isBooleanStorage(type));
  if (!type.record || isOwnedResource(module, type))
    return false;
  const auto &record = module.records[*type.record];
  return !record.error && !record.uninhabited && record.variants.empty() &&
         !record.fields.empty() &&
         std::ranges::all_of(record.fields, [](const auto &f) { return scalar(f.type); });
}
// A pure traversal inside an I/O/error-handling function still needs one native
// loop, rather than one checked kernel call per arithmetic operation. Outline
// only a fully proven scalar region; publish its local scalar updates once on
// success. Fault/cancellation aborts the containing function, so those private
// intermediate scalar assignments have no outside observer.
bool extract(TypedModule &module, FunctionId owner, std::size_t begin, std::size_t end) {
  const auto original = module.functions[owner];
  std::vector<TypedStatement> statements(original.body.begin() + begin,
                                         original.body.begin() + end + 1);
  auto proof = graph(module, roots(statements));
  for (const auto &statement : statements)
    if (statement.binding)
      proof.writes.insert(*statement.binding);
  std::set<SymbolId> before(original.parameters.begin(), original.parameters.end());
  for (std::size_t n = 0; n < begin; ++n)
    if (original.body[n].binding)
      before.insert(*original.body[n].binding);
  std::vector<SymbolId> parameters;
  for (auto symbol : proof.reads)
    if (!proof.writes.contains(symbol) || before.contains(symbol)) {
      if (!input(module, module.symbols[symbol]))
        return false;
      parameters.push_back(symbol);
    }
  std::vector<ValueId> later;
  for (std::size_t n = end + 1; n < original.body.size(); ++n)
    later.push_back(original.body[n].value);
  if (!proof.values.contains(original.result))
    later.push_back(original.result);
  const auto used = graph(module, std::move(later));
  std::vector<SymbolId> outputs;
  for (auto symbol : proof.writes)
    if (used.reads.contains(symbol)) {
      if (!scalar(module.symbols[symbol].type))
        return false;
      outputs.push_back(symbol);
    }
  const bool returns = proof.values.contains(original.result);
  if (returns && !scalar(module.expressions[original.result].type))
    return false;
  if (parameters.size() > 16 || outputs.size() + unsigned(returns) > 16)
    return false;
  const auto valueCount = module.expressions.size(), symbolCount = module.symbols.size();
  const auto recordCount = module.records.size(), functionCount = module.functions.size();
  const auto rollback = [&] {
    module.expressions.resize(valueCount);
    module.symbols.resize(symbolCount);
    module.records.resize(recordCount);
    module.functions.resize(functionCount);
  };
  const auto helper = module.functions.size();
  module.functions.emplace_back();
  std::map<SymbolId, SymbolId> symbols;
  const auto symbol = [&](SymbolId id) {
    if (auto found = symbols.find(id); found != symbols.end())
      return found->second;
    auto copied = module.symbols[id];
    copied.owner = helper;
    copied.parameter = false;
    const auto target = module.symbols.size();
    module.symbols.push_back(std::move(copied));
    symbols[id] = target;
    return target;
  };
  std::vector<SymbolId> formal;
  for (auto id : parameters) {
    const auto target = symbol(id);
    module.symbols[target].parameter = true;
    formal.push_back(target);
    module.symbols[target].mutableAccess = false;
    module.symbols[target].owned = false;
  }
  std::map<ValueId, ValueId> values;
  std::vector<ValueId> expressions;
  const auto add = [&](TypedExpr node) {
    const auto id = module.expressions.size();
    module.expressions.push_back(std::move(node));
    expressions.push_back(id);
    return id;
  };
  std::function<ValueId(ValueId)> clone = [&](ValueId id) {
    if (auto found = values.find(id); found != values.end())
      return found->second;
    auto node = module.expressions[id];
    visitTypedChildren(
        node, [&](ValueId &child) { child = clone(child); },
        [&](SymbolId &binding) { binding = symbol(binding); });
    if (auto *read = std::get_if<SymbolRead>(&node.operation))
      read->symbol = symbol(read->symbol);
    return values[id] = add(std::move(node));
  };
  for (auto &statement : statements) {
    statement.value = clone(statement.value);
    if (statement.binding)
      statement.binding = symbol(*statement.binding);
  }
  const auto span = statements.front().span;
  std::vector<ValueId> results;
  for (auto id : outputs)
    results.push_back(add({span, module.symbols[id].type, SymbolRead{symbol(id)}}));
  if (returns)
    results.push_back(clone(original.result));
  ValueType resultType;
  resultType.builtin = BuiltinValue::Unit;
  ValueId result;
  if (results.empty()) {
    result = add({span, resultType, BoolLiteral{false}});
  } else if (results.size() == 1) {
    result = results[0];
    resultType = module.expressions[result].type;
  } else {
    const auto record = module.records.size();
    TypedRecord layout{"__iteration_results_" + std::to_string(helper), span, {}};
    std::vector<RecordFieldValue> fields;
    for (std::size_t n = 0; n < results.size(); ++n) {
      layout.fields.push_back(
          {"value" + std::to_string(n), module.expressions[results[n]].type, span});
      fields.push_back({n, results[n]});
    }
    resultType.builtin = BuiltinValue::Record;
    resultType.record = record;
    resultType.recordName = layout.name;
    module.records.push_back(std::move(layout));
    result = add({span, resultType, RecordValue{{}, std::move(fields), {}}});
  }
  statements.push_back({span, {}, result});
  auto &fn = module.functions[helper];
  fn.name = "__iteration_region_" + std::to_string(helper);
  fn.declaration = span;
  fn.parameters = std::move(formal);
  fn.resultType = resultType;
  fn.declaredResult = resultType;
  fn.result = result;
  fn.body = std::move(statements);
  fn.expressions = std::move(expressions);
  fn.anonymous = true;
  if (!fuseLocalIteratorFunction(module, helper) ||
      continuation::resumableFunctions(module)[helper]) {
    rollback();
    return false;
  }
  // Only a bulk native region may be introduced here. Otherwise outlining
  // would merely move per-item continuation overhead into another function.
  auto planned = continuation::plan(module, {.entry = helper, .valueScopes = true});
  if (!planned || std::ranges::any_of(planned->plans[helper]->code, [](const auto &op) {
        return op.code == continuation::Code::Jump || op.code == continuation::Code::Branch;
      })) {
    rollback();
    return false;
  }
  std::vector<ValueId> arguments;
  for (auto id : parameters)
    arguments.push_back(add({span, module.symbols[id].type, SymbolRead{id}}));
  const auto call = add({span, resultType, FunctionCall{helper, std::move(arguments)}});
  std::vector<TypedStatement> replacement;
  ValueId newResult = original.result;
  if (results.size() <= 1) {
    replacement.push_back({span, outputs.empty() ? std::optional<SymbolId>{} : outputs[0], call});
    if (returns)
      newResult = call;
  } else {
    const auto saved = module.symbols.size();
    module.symbols.push_back(
        {"__iteration_values_" + std::to_string(saved), resultType, span, owner, false});
    replacement.push_back({span, saved, call});
    for (std::size_t n = 0; n < results.size(); ++n) {
      const auto read = add({span, resultType, SymbolRead{saved}});
      const auto field = add({span, module.expressions[results[n]].type, RecordFieldRead{read, n}});
      replacement.push_back(
          {span, n < outputs.size() ? std::optional<SymbolId>(outputs[n]) : std::nullopt, field});
      if (n == outputs.size() && returns)
        newResult = field;
    }
  }
  auto body = original.body;
  body.erase(body.begin() + begin, body.begin() + end + 1);
  body.insert(body.begin() + begin, replacement.begin(), replacement.end());
  auto aliveRoots = roots(body);
  aliveRoots.push_back(newResult);
  auto alive = graph(module, std::move(aliveRoots));
  module.functions[owner].body = std::move(body);
  module.functions[owner].result = newResult;
  module.functions[owner].expressions.assign(alive.values.begin(), alive.values.end());
  return true;
}
} // namespace
void extractLocalIteratorRegions(TypedModule &module) {
  const auto count = module.functions.size();
  unsigned admitted = 0;
  for (FunctionId owner = 0; owner < count && admitted < 8; ++owner) {
    if (module.functions[owner].generator || module.functions[owner].compileTimeOnly)
      continue;
    for (unsigned region = 0; region < 4 && admitted < 8; ++region) {
      const auto body = module.functions[owner].body;
      bool changed = false;
      for (std::size_t begin = 0; begin < body.size() && !changed; ++begin) {
        if (!body[begin].binding || !isIterator(module, module.expressions[body[begin].value].type))
          continue;
        for (std::size_t end = begin + 1; end < body.size() && end - begin < 64; ++end) {
          const auto *loop =
              std::get_if<WhileOperation>(&module.expressions[body[end].value].operation);
          if (!loop || loop->collecting || loop->initial.size() != 1 ||
              !isIterator(module, module.expressions[loop->initial[0].value].type))
            continue;
          if (extract(module, owner, begin, end)) {
            ++admitted;
            changed = true;
          }
          break;
        }
      }
      if (!changed)
        break;
    }
  }
}
} // namespace kalandra
