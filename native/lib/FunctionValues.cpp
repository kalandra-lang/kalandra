#include "kalandra/Continuation.h"
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace kalandra {
bool sameFunctionValueType(const ValueType &a, const ValueType &b, std::size_t depth) {
  std::size_t remaining = 300000;
  const auto compare = [&](auto &&self, const ValueType &a, const ValueType &b,
                           std::size_t depth) -> bool {
    if (!remaining)
      return false;
    --remaining;
    if (depth >= 128 || a.builtin != b.builtin || a.tensor != b.tensor || a.array != b.array ||
        a.task != b.task || a.fallible != b.fallible || a.device != b.device ||
        a.record != b.record || a.errorRecord != b.errorRecord ||
        a.shape.size() != b.shape.size() ||
        (a.builtin == BuiltinValue::Numeric && a.element != b.element))
      return false;
    for (std::size_t i = 0; i < a.shape.size(); ++i)
      if (a.shape[i].constant != b.shape[i].constant)
        return false;
    if (a.builtin != BuiltinValue::Function)
      return !a.callable && !b.callable;
    if (!a.callable || !b.callable ||
        a.callable->parameters.size() != b.callable->parameters.size())
      return false;
    for (std::size_t i = 0; i < a.callable->parameters.size(); ++i)
      if (!self(self, a.callable->parameters[i], b.callable->parameters[i], depth + 1))
        return false;
    return self(self, a.callable->result, b.callable->result, depth + 1);
  };
  return compare(compare, a, b, depth);
}

bool workerClosureCaptureTypeSupported(const TypedModule &module, const ValueType &root) {
  std::vector<ValueType> pending{root};
  std::unordered_set<std::size_t> visited;
  while (!pending.empty()) {
    const auto value = pending.back();
    pending.pop_back();
    if (value.task || value.fallible || value.device || value.errorRecord ||
        (value.builtin != BuiltinValue::Numeric && value.builtin != BuiltinValue::Bool &&
         value.builtin != BuiltinValue::String && value.builtin != BuiltinValue::Record &&
         value.builtin != BuiltinValue::Function && value.builtin != BuiltinValue::TaskError))
      return false;
    if (value.builtin == BuiltinValue::Function &&
        (!value.callable || (value.tensor && !isFunctionArray(value))))
      return false;
    if (value.builtin == BuiltinValue::Record) {
      if (!value.record || *value.record >= module.records.size())
        return false;
      if (visited.insert(*value.record).second)
        for (const auto &field : module.records[*value.record].fields)
          pending.push_back(field.type);
    }
  }
  return true;
}
bool containsFunctionStorage(const TypedModule &module, const ValueType &type) {
  std::vector<const ValueType *> pending{&type};
  std::unordered_set<std::size_t> visited;
  while (!pending.empty()) {
    const auto &value = *pending.back();
    pending.pop_back();
    if (value.builtin == BuiltinValue::Function)
      return true;
    for (auto record : {value.record, value.errorRecord})
      if (record && *record < module.records.size() && visited.insert(*record).second)
        for (const auto &field : module.records[*record].fields)
          pending.push_back(&field.type);
  }
  return false;
}
namespace {
// A bounded, whole-module may-flow graph. Aggregate projections deliberately
// overapproximate fields/elements, but callable edges require exact signatures.
class WorkerValueFlow {
public:
  WorkerValueFlow(const TypedModule &module, const std::unordered_map<SymbolId, ValueId> &bindings,
                  std::size_t budget = 300000)
      : targets(module.expressions.size()), outcomes(module.functions.size()), module_(module),
        bindings_(bindings), edges_(module.expressions.size()), uses_(module.expressions.size()),
        references_(module.functions.size()), remaining_(budget) {}

  std::unordered_map<SymbolId, std::unordered_set<ValueId>> parameterSources;
  std::unordered_map<ValueId, std::unordered_set<FunctionId>> invocationTargets;
  std::vector<std::unordered_set<FunctionId>> targets;
  std::vector<std::vector<ValueId>> outcomes;

  bool unknown(ValueId id) const {
    return id >= targets.size() || targets[id].contains(module_.functions.size());
  }

  bool build() {
    // Join possible application-error producers, including early guard failures.
    // This is deliberately conservative across locally handled channels too.
    for (FunctionId fn = 0; fn < module_.functions.size(); ++fn)
      for (auto id : module_.functions[fn].expressions) {
        if (id >= module_.expressions.size() || !step())
          return false;
        const auto &type = module_.expressions[id].type;
        ValueType error;
        error.builtin = BuiltinValue::Record;
        error.record = type.errorRecord;
        if (type.errorRecord && containsFunctionStorage(module_, error))
          outcomes[fn].push_back(id);
      }
    for (ValueId id = 0; id < module_.expressions.size(); ++id) {
      const auto &value = module_.expressions[id];
      if (const auto *read = std::get_if<SymbolRead>(&value.operation)) {
        if (read->symbol >= module_.symbols.size())
          return false;
        reads_[read->symbol].push_back(id);
      }
      if (const auto *ref = std::get_if<FunctionReference>(&value.operation)) {
        if (ref->function >= module_.functions.size())
          return false;
        references_[ref->function] = id;
      }
    }
    for (ValueId id = 0; id < module_.expressions.size() && valid_; ++id) {
      const auto &value = module_.expressions[id];
      if (const auto *ref = std::get_if<FunctionReference>(&value.operation)) {
        const auto &fn = module_.functions[ref->function];
        if (fn.captureCount > fn.parameters.size() || ref->captures.size() != fn.captureCount)
          return false;
        seed(id, ref->function);
        const auto explicitCount = fn.parameters.size() - fn.captureCount;
        for (std::size_t i = 0; i < fn.captureCount; ++i)
          source(fn.parameters[explicitCount + i], ref->captures[i]);
      } else if (const auto *call = std::get_if<FunctionCall>(&value.operation)) {
        if (call->function >= module_.functions.size())
          return false;
        const auto &fn = module_.functions[call->function];
        if (fn.parameters.size() != call->arguments.size())
          return false;
        for (std::size_t i = 0; i < fn.parameters.size(); ++i)
          source(fn.parameters[i], call->arguments[i]);
        edge(fn.result, id);
        for (auto outcome : outcomes[call->function])
          edge(outcome, id);
      } else if (const auto *call = std::get_if<FunctionInvoke>(&value.operation)) {
        if (call->callee >= id)
          return false;
        uses_[call->callee].push_back(id);
      } else if (const auto *map = std::get_if<ArrayMap>(&value.operation)) {
        if (map->function >= module_.functions.size())
          return false;
        const auto &fn = module_.functions[map->function];
        if (fn.parameters.size() != map->captures.size() + 1)
          return false;
        source(fn.parameters[0], map->input);
        for (std::size_t i = 0; i < map->captures.size(); ++i)
          source(fn.parameters[i + 1], map->captures[i]);
        edge(fn.result, id);
        if (map->parallel)
          for (auto outcome : outcomes[map->function])
            edge(outcome, id);
      } else if (const auto *read = std::get_if<SymbolRead>(&value.operation)) {
        if (!module_.symbols[read->symbol].parameter) {
          const auto found = bindings_.find(read->symbol);
          if (found == bindings_.end() || found->second >= id)
            seed(id, module_.functions.size());
          else
            edge(found->second, id);
        }
      } else if (const auto *record = std::get_if<RecordValue>(&value.operation)) {
        if (record->base)
          edge(*record->base, id);
        for (const auto &field : record->fields)
          edge(field.value, id);
      } else if (const auto *array = std::get_if<TensorLiteral>(&value.operation)) {
        for (auto element : array->elements)
          edge(element, id);
      } else if (const auto *narrow = std::get_if<ErrorNarrow>(&value.operation)) {
        edge(narrow->input, id);
      } else if (const auto *field = std::get_if<RecordFieldRead>(&value.operation)) {
        edge(field->input, id);
      } else if (const auto *index = std::get_if<ArrayIndex>(&value.operation)) {
        edge(index->input, id);
      } else if (const auto *slice = std::get_if<ArraySlice>(&value.operation)) {
        edge(slice->input, id);
      } else if (const auto *concat = std::get_if<ArrayConcat>(&value.operation)) {
        edge(concat->left, id);
        edge(concat->right, id);
      } else if (const auto *shape = std::get_if<ShapeCheck>(&value.operation)) {
        edge(shape->input, id);
      } else if (const auto *task = std::get_if<TaskOperation>(&value.operation)) {
        edge(task->operand, id);
      } else if (const auto *group = std::get_if<ParallelOperation>(&value.operation)) {
        edge(group->result, id);
      } else if (const auto *caught = std::get_if<CatchOperation>(&value.operation)) {
        source(caught->parameter, caught->operand);
        edge(caught->operand, id);
        edge(caught->result, id);
      } else if (const auto *choice = std::get_if<ConditionalOperation>(&value.operation)) {
        edge(choice->trueResult, id);
        edge(choice->falseResult, id);
      } else if (const auto *match = std::get_if<MatchOperation>(&value.operation)) {
        for (const auto &arm : match->cases)
          edge(arm.result, id);
      } else if (const auto *failure = std::get_if<FailOperation>(&value.operation)) {
        edge(failure->error, id);
      } else {
        seed(id, module_.functions.size());
      }
    }
    drain();
    // Missing sources and unresolved calls must not silently disappear from a
    // partial target set. Unknown callees poison every compatible target's
    // incoming parameters, preserving conservative checking of all callers.
    for (const auto &[symbol, reads] : reads_)
      if (module_.symbols[symbol].parameter && parameterSources[symbol].empty())
        for (auto id : reads)
          seed(id, module_.functions.size());
    for (ValueId id = 0; id < module_.expressions.size(); ++id)
      if (const auto *call = std::get_if<FunctionInvoke>(&module_.expressions[id].operation))
        if (invocationTargets[id].empty())
          seed(call->callee, module_.functions.size());
    drain();
    return valid_;
  }

private:
  const TypedModule &module_;
  const std::unordered_map<SymbolId, ValueId> &bindings_;
  std::unordered_map<SymbolId, std::vector<ValueId>> reads_;
  std::vector<std::unordered_set<ValueId>> edges_;
  std::vector<std::vector<ValueId>> uses_;
  std::vector<std::optional<ValueId>> references_;
  std::vector<std::pair<ValueId, FunctionId>> pending_;
  std::size_t next_ = 0;
  std::size_t remaining_ = 300000;
  bool valid_ = true;

  bool step() {
    if (!remaining_) {
      valid_ = false;
      return false;
    }
    --remaining_;
    return valid_;
  }
  void seed(ValueId id, FunctionId target) {
    if (!step() || id >= targets.size()) {
      valid_ = false;
      return;
    }
    if (containsFunctionStorage(module_, module_.expressions[id].type) &&
        targets[id].insert(target).second)
      pending_.emplace_back(id, target);
  }
  void edge(ValueId from, ValueId to) {
    if (!step() || from >= targets.size() || to >= targets.size()) {
      valid_ = false;
      return;
    }
    if (!containsFunctionStorage(module_, module_.expressions[to].type) ||
        !edges_[from].insert(to).second)
      return;
    const auto previous = targets[from];
    for (auto target : previous)
      seed(to, target);
  }
  void source(SymbolId symbol, ValueId value) {
    if (!step() || symbol >= module_.symbols.size() || value >= targets.size()) {
      valid_ = false;
      return;
    }
    if (parameterSources[symbol].insert(value).second)
      for (auto read : reads_[symbol])
        edge(value, read);
  }
  bool compatible(FunctionId fn, ValueId callee) const {
    return fn < references_.size() && references_[fn] &&
           sameFunctionValueType(module_.expressions[*references_[fn]].type,
                                 module_.expressions[callee].type);
  }
  void connect(ValueId id, FunctionId target) {
    const auto &call = std::get<FunctionInvoke>(module_.expressions[id].operation);
    if (!compatible(target, call.callee) || !invocationTargets[id].insert(target).second)
      return;
    const auto &fn = module_.functions[target];
    if (fn.captureCount > fn.parameters.size() ||
        fn.parameters.size() - fn.captureCount != call.arguments.size()) {
      valid_ = false;
      return;
    }
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
      source(fn.parameters[i], call.arguments[i]);
    }
    edge(fn.result, id);
    for (auto outcome : outcomes[target])
      edge(outcome, id);
  }
  void drain() {
    while (next_ < pending_.size() && step()) {
      const auto [id, target] = pending_[next_++];
      for (auto to : edges_[id])
        seed(to, target);
      for (auto call : uses_[id]) {
        if (target < module_.functions.size()) {
          connect(call, target);
        } else {
          seed(call, target);
          for (FunctionId fn = 0; fn < references_.size() && step(); ++fn)
            if (compatible(fn, id)) {
              connect(call, fn);
              for (auto symbol : module_.functions[fn].parameters)
                for (auto read : reads_[symbol])
                  seed(read, target);
            }
        }
      }
    }
  }
};

std::unordered_map<SymbolId, ValueId> valueBindings(const TypedModule &module) {
  std::unordered_map<SymbolId, ValueId> bindings;
  const auto statements = [&](const auto &body) {
    for (const auto &statement : body)
      if (statement.binding)
        bindings.emplace(*statement.binding, statement.value);
  };
  for (const auto &fn : module.functions)
    statements(fn.body);
  for (const auto &expression : module.expressions)
    std::visit(
        [&](const auto &op) {
          if constexpr (requires { op.body; })
            statements(op.body);
          else if constexpr (std::is_same_v<std::decay_t<decltype(op)>, ConditionalOperation>) {
            statements(op.whenTrue);
            statements(op.whenFalse);
          } else if constexpr (std::is_same_v<std::decay_t<decltype(op)>, MatchOperation>)
            for (const auto &arm : op.cases)
              statements(arm.body);
        },
        expression.operation);
  return bindings;
}

bool workerValueProvenance(const TypedModule &module, ValueId root) {
  const auto bindings = valueBindings(module);
  WorkerValueFlow flow(module, bindings);
  if (!flow.build())
    return false;
  std::vector<ValueId> pending{root};
  std::unordered_set<ValueId> visited;
  std::size_t budget = 300000;
  bool found = false;
  while (!pending.empty()) {
    if (!budget--)
      return false;
    const auto id = pending.back();
    pending.pop_back();
    if (id >= module.expressions.size())
      return false;
    if (!visited.insert(id).second)
      continue;
    const auto &value = module.expressions[id];
    if (value.type.device || flow.unknown(id))
      return false;
    if (!containsFunctionStorage(module, value.type))
      continue;
    if (const auto *ref = std::get_if<FunctionReference>(&value.operation)) {
      if (ref->function >= module.functions.size())
        return false;
      const auto &fn = module.functions[ref->function];
      if (ref->captures.size() != fn.captureCount)
        return false;
      for (auto capture : ref->captures) {
        if (capture >= module.expressions.size() ||
            !workerClosureCaptureTypeSupported(module, module.expressions[capture].type))
          return false;
        if (containsFunctionStorage(module, module.expressions[capture].type))
          pending.push_back(capture);
      }
      found = true;
    } else if (const auto *record = std::get_if<RecordValue>(&value.operation)) {
      if (record->base)
        pending.push_back(*record->base);
      for (const auto &field : record->fields)
        pending.push_back(field.value);
      found = true;
    } else if (const auto *array = std::get_if<TensorLiteral>(&value.operation)) {
      for (auto element : array->elements)
        pending.push_back(element);
      found = true;
    } else if (const auto *narrow = std::get_if<ErrorNarrow>(&value.operation)) {
      pending.push_back(narrow->input);
    } else if (const auto *field = std::get_if<RecordFieldRead>(&value.operation)) {
      pending.push_back(field->input);
    } else if (const auto *index = std::get_if<ArrayIndex>(&value.operation)) {
      pending.push_back(index->input);
    } else if (const auto *slice = std::get_if<ArraySlice>(&value.operation)) {
      pending.push_back(slice->input);
    } else if (const auto *concat = std::get_if<ArrayConcat>(&value.operation)) {
      pending.push_back(concat->left);
      pending.push_back(concat->right);
    } else if (const auto *shape = std::get_if<ShapeCheck>(&value.operation)) {
      pending.push_back(shape->input);
    } else if (const auto *call = std::get_if<FunctionCall>(&value.operation)) {
      if (call->function >= module.functions.size())
        return false;
      pending.push_back(module.functions[call->function].result);
      pending.insert(pending.end(), flow.outcomes[call->function].begin(),
                     flow.outcomes[call->function].end());
      if (!containsFunctionStorage(
              module, module.expressions[module.functions[call->function].result].type) &&
          flow.outcomes[call->function].empty())
        found = true;
    } else if (const auto *call = std::get_if<FunctionInvoke>(&value.operation)) {
      const auto candidates = flow.invocationTargets.find(id);
      if (call->callee >= id || candidates == flow.invocationTargets.end() ||
          candidates->second.empty())
        return false;
      if (flow.unknown(call->callee))
        return false;
      for (auto fn : candidates->second) {
        pending.push_back(module.functions[fn].result);
        pending.insert(pending.end(), flow.outcomes[fn].begin(), flow.outcomes[fn].end());
        if (!containsFunctionStorage(module,
                                     module.expressions[module.functions[fn].result].type) &&
            flow.outcomes[fn].empty())
          found = true;
      }
    } else if (const auto *map = std::get_if<ArrayMap>(&value.operation)) {
      if (map->function >= module.functions.size())
        return false;
      pending.push_back(module.functions[map->function].result);
      if (map->parallel)
        pending.insert(pending.end(), flow.outcomes[map->function].begin(),
                       flow.outcomes[map->function].end());
      pending.push_back(map->input);
      pending.insert(pending.end(), map->captures.begin(), map->captures.end());
    } else if (const auto *task = std::get_if<TaskOperation>(&value.operation)) {
      if (task->operand >= id)
        return false;
      pending.push_back(task->operand);
    } else if (const auto *group = std::get_if<ParallelOperation>(&value.operation)) {
      if (group->result >= id)
        return false;
      pending.push_back(group->result);
    } else if (const auto *caught = std::get_if<CatchOperation>(&value.operation)) {
      if (caught->operand >= id || caught->result >= id)
        return false;
      pending.push_back(caught->operand);
      pending.push_back(caught->result);
    } else if (const auto *failure = std::get_if<FailOperation>(&value.operation);
               failure && value.type.fallible) {
      if (failure->error >= id)
        return false;
      pending.push_back(failure->error);
      found = true;
    } else if (const auto *read = std::get_if<SymbolRead>(&value.operation)) {
      const auto binding = bindings.find(read->symbol);
      if (read->symbol >= module.symbols.size())
        return false;
      if (module.symbols[read->symbol].parameter) {
        const auto sources = flow.parameterSources.find(read->symbol);
        if (sources == flow.parameterSources.end() || sources->second.empty())
          return false;
        pending.insert(pending.end(), sources->second.begin(), sources->second.end());
      } else {
        if (binding != bindings.end()) {
          if (binding->second >= id)
            return false;
          pending.push_back(binding->second);
        } else {
          const auto sources = flow.parameterSources.find(read->symbol);
          if (sources == flow.parameterSources.end() || sources->second.empty())
            return false;
          pending.insert(pending.end(), sources->second.begin(), sources->second.end());
        }
      }
    } else if (const auto *choice = std::get_if<ConditionalOperation>(&value.operation)) {
      if (choice->trueResult >= id || choice->falseResult >= id)
        return false;
      pending.push_back(choice->trueResult);
      pending.push_back(choice->falseResult);
    } else if (const auto *match = std::get_if<MatchOperation>(&value.operation)) {
      if (match->cases.empty())
        return false;
      for (const auto &arm : match->cases) {
        if (arm.result >= id)
          return false;
        pending.push_back(arm.result);
      }
    } else
      return false;
  }
  return found;
}
} // namespace
std::optional<FunctionInvocationFlow> functionInvocationFlow(const TypedModule &module,
                                                             std::size_t budget) {
  const auto bindings = valueBindings(module);
  WorkerValueFlow flow(module, bindings, budget);
  if (!flow.build())
    return {};
  FunctionInvocationFlow result;
  result.targets.resize(module.expressions.size());
  result.unknown.resize(module.expressions.size());
  result.reachable.resize(module.expressions.size());
  for (ValueId id = 0; id < module.expressions.size(); ++id)
    if (const auto *call = std::get_if<FunctionInvoke>(&module.expressions[id].operation)) {
      result.unknown[id] = flow.unknown(call->callee);
      if (const auto found = flow.invocationTargets.find(id);
          found != flow.invocationTargets.end()) {
        result.targets[id].assign(found->second.begin(), found->second.end());
        std::ranges::sort(result.targets[id]);
      }
    }
  std::vector<bool> visited(module.functions.size());
  std::vector<FunctionId> pending;
  for (FunctionId id = 0; id < module.functions.size(); ++id)
    if (module.functions[id].name == "main")
      pending.push_back(id);
  while (!pending.empty()) {
    if (!budget--)
      return {};
    const auto id = pending.back();
    pending.pop_back();
    if (visited.at(id))
      continue;
    visited[id] = true;
    for (auto value : module.functions[id].expressions) {
      if (!budget--)
        return {};
      result.reachable.at(value) = true;
      const auto &op = module.expressions[value].operation;
      if (const auto *call = std::get_if<FunctionCall>(&op))
        pending.push_back(call->function);
      else if (const auto *map = std::get_if<ArrayMap>(&op))
        pending.push_back(map->function);
      else if (std::holds_alternative<FunctionInvoke>(op))
        pending.insert(pending.end(), result.targets[value].begin(), result.targets[value].end());
    }
  }
  return result;
}
std::optional<std::string> functionValueTransportError(const TypedModule &module,
                                                       const TypedExpr &invocation) {
  const bool indirect = std::holds_alternative<FunctionInvoke>(invocation.operation);
  const bool map = std::holds_alternative<ArrayMap>(invocation.operation);
  const bool collection = std::holds_alternative<ArraySlice>(invocation.operation) ||
                          std::holds_alternative<ArrayConcat>(invocation.operation) ||
                          std::holds_alternative<CollectionQuery>(invocation.operation);
  if (containsFunctionStorage(module, invocation.type)) {
    const auto found = std::find_if(module.expressions.begin(), module.expressions.end(),
                                    [&](const auto &value) { return &value == &invocation; });
    if (found == module.expressions.end() ||
        !workerValueProvenance(module, found - module.expressions.begin()))
      return "function-containing worker outcomes require proven CPU closure captures; "
             "unproven success/error provenance is not implemented yet";
  }
  const auto arguments = std::visit(
      [](const auto &op) -> std::vector<ValueId> {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, FunctionCall> || std::is_same_v<T, RuntimeInput>)
          return op.arguments;
        else if constexpr (std::is_same_v<T, FunctionInvoke>) {
          auto args = op.arguments;
          args.insert(args.begin(), op.callee);
          return args;
        } else if constexpr (std::is_same_v<T, ArrayMap>) {
          auto args = op.captures;
          args.push_back(op.input);
          return args;
        } else if constexpr (requires { op.input; })
          return {op.input};
        else if constexpr (requires {
                             op.left;
                             op.right;
                           })
          return {op.left, op.right};
        else
          return {};
      },
      invocation.operation);
  for (auto id : arguments) {
    if (id >= module.expressions.size())
      return "worker argument has invalid provenance";
    const auto &type = module.expressions[id].type;
    if (!containsFunctionStorage(module, type))
      continue;
    if ((std::holds_alternative<FunctionCall>(invocation.operation) || indirect || map ||
         collection) &&
        !type.task && !type.fallible && workerValueProvenance(module, id))
      continue;
    return "function-valued worker arguments require proven CPU closure captures; unproven "
           "capture provenance is not implemented yet";
  }
  return {};
}

std::optional<std::string> functionValueCapabilityError(const TypedModule &module,
                                                        NumericTarget target) {
  bool present = false;
  const bool placed = hasPlacement(module);
  std::size_t visited = 0;
  const auto valid = [&](auto &&self, const ValueType &type, std::size_t depth) -> bool {
    if (type.builtin != BuiltinValue::Function)
      return !type.callable;
    if (++visited > 300000 || depth >= 128)
      return false;
    present = true;
    const bool array = isFunctionArray(type);
    if (target != NumericTarget::Cpu || placed || !type.callable ||
        (array
             ? (!type.tensor || type.shape.size() != 1 ||
                (type.shape[0].constant && *type.shape[0].constant > std::uint64_t(INT64_MAX) / 16))
             : (type.tensor || !type.shape.empty())) ||
        type.device || type.record ||
        (type.errorRecord && (!type.fallible || *type.errorRecord >= module.records.size() ||
                              !module.records[*type.errorRecord].error)))
      return false;
    if (type.callable->parameters.size() > 300000 - visited)
      return false;
    visited += type.callable->parameters.size();
    const auto plain = [&](const ValueType &value) {
      const bool collection =
          value.tensor ? (value.shape.size() <= 128 &&
                          std::ranges::all_of(
                              value.shape,
                              [](const auto &extent) {
                                return !extent.constant ||
                                       *extent.constant <=
                                           std::uint64_t(std::numeric_limits<std::int64_t>::max());
                              }) &&
                          (!value.array || (value.shape.size() == 1 && !value.shape[0].constant)) &&
                          (value.builtin == BuiltinValue::Numeric || isOwnedArray(value)))
                       : (!value.array && value.shape.empty());
      return collection && !value.task && !value.fallible && !value.device && !value.errorRecord &&
             (value.builtin == BuiltinValue::Numeric || value.builtin == BuiltinValue::Bool ||
              value.builtin == BuiltinValue::String || value.builtin == BuiltinValue::Record ||
              value.builtin == BuiltinValue::TaskError ||
              value.builtin == BuiltinValue::FaultKind ||
              value.builtin == BuiltinValue::Function) &&
             (value.builtin == BuiltinValue::Record
                  ? value.record && *value.record < module.records.size()
                  : !value.record) &&
             (value.builtin != BuiltinValue::Numeric ||
              numericScalarArithmeticAvailable(value.element, NumericTarget::Cpu)) &&
             self(self, value, depth + 1);
    };
    auto result = type.callable->result;
    if (result.errorRecord && (!result.fallible || *result.errorRecord >= module.records.size() ||
                               !module.records[*result.errorRecord].error))
      return false;
    result.fallible = false;
    result.errorRecord.reset();
    return plain(result) && std::ranges::all_of(type.callable->parameters, plain);
  };
  for (const auto &record : module.records)
    for (const auto &field : record.fields)
      if (!valid(valid, field.type, 0))
        return "invalid or unsupported function-valued field type";
  for (const auto &symbol : module.symbols)
    if (!valid(valid, symbol.type, 0))
      return "invalid or unsupported function-value type";
  for (const auto &fn : module.functions) {
    if (!valid(valid, fn.resultType, 0) ||
        (fn.declaredResult && !valid(valid, *fn.declaredResult, 0)))
      return "invalid or unsupported function-value result";
  }
  for (const auto &expression : module.expressions) {
    present |= std::holds_alternative<FunctionReference>(expression.operation) ||
               std::holds_alternative<FunctionInvoke>(expression.operation);
    if (!valid(valid, expression.type, 0))
      return "invalid or unsupported function-value type";
  }
  if (!present)
    return {};
  // Array lengths are value facts; compare stored element signatures separately.
  const auto sameStorageType = [](ValueType a, ValueType b) {
    if (isFunctionArray(a) && isFunctionArray(b) && a.shape.size() == 1 && b.shape.size() == 1) {
      a.shape[0] = b.shape[0] = Extent{};
    }
    return sameFunctionValueType(a, b);
  };
  const auto completed = [](ValueType type) {
    type.fallible = false;
    type.errorRecord.reset();
    return type;
  };
  const auto bindings = [&](const std::vector<TypedStatement> &body) {
    for (const auto &statement : body) {
      if (statement.value >= module.expressions.size() ||
          (statement.binding && *statement.binding >= module.symbols.size()))
        return false;
      if (statement.binding) {
        const auto &symbol = module.symbols[*statement.binding].type;
        const auto &value = module.expressions[statement.value].type;
        if ((symbol.builtin == BuiltinValue::Function || value.builtin == BuiltinValue::Function) &&
            !sameStorageType(symbol, value))
          return false;
      }
    }
    return true;
  };
  for (const auto &fn : module.functions) {
    if (fn.captureCount > fn.parameters.size() || !bindings(fn.body) ||
        std::ranges::any_of(fn.expressions,
                            [&](auto id) { return id >= module.expressions.size(); }))
      return "function has invalid expression/binding metadata";
    if (fn.result >= module.expressions.size())
      return "function has an invalid result";
    if (fn.resultType.builtin == BuiltinValue::Function &&
        (!sameStorageType(completed(fn.resultType),
                          completed(module.expressions[fn.result].type)) ||
         (fn.declaredResult && !sameStorageType(fn.resultType, *fn.declaredResult))))
      return "function changes its returned callable signature";
  }
  for (ValueId id = 0; id < module.expressions.size(); ++id) {
    const auto &expression = module.expressions[id];
    if (const auto *choice = std::get_if<ConditionalOperation>(&expression.operation);
        choice && (!bindings(choice->whenTrue) || !bindings(choice->whenFalse)))
      return "conditional changes a function binding signature";
    if (const auto *caught = std::get_if<CatchOperation>(&expression.operation);
        caught && !bindings(caught->body))
      return "catch changes a function binding signature";
    if (const auto *match = std::get_if<MatchOperation>(&expression.operation))
      for (const auto &arm : match->cases)
        if (!bindings(arm.body))
          return "match changes a function binding signature";
    if (const auto *guard = std::get_if<GuardOperation>(&expression.operation);
        guard && !bindings(guard->body))
      return "guard changes a function binding signature";
    if (const auto *group = std::get_if<ParallelOperation>(&expression.operation);
        group && !bindings(group->body))
      return "parallel group changes a function binding signature";
    if (expression.type.builtin == BuiltinValue::Function) {
      if (const auto *read = std::get_if<SymbolRead>(&expression.operation)) {
        if (read->symbol >= module.symbols.size() ||
            !sameStorageType(expression.type, module.symbols[read->symbol].type))
          return "function read changes its binding signature";
      } else if (const auto *read = std::get_if<RecordFieldRead>(&expression.operation)) {
        if (read->input >= id)
          return "function field read requires a preceding aggregate";
        const auto &input = module.expressions[read->input].type;
        if (input.builtin != BuiltinValue::Record || !input.record ||
            *input.record >= module.records.size() || input.tensor || input.array || input.task ||
            input.fallible || input.device ||
            read->field >= module.records[*input.record].fields.size() ||
            !sameStorageType(expression.type,
                             module.records[*input.record].fields[read->field].type))
          return "function field read changes its declared signature";
      } else if (const auto *choice = std::get_if<ConditionalOperation>(&expression.operation)) {
        if (choice->trueResult >= id || choice->falseResult >= id ||
            !sameStorageType(completed(expression.type),
                             completed(module.expressions[choice->trueResult].type)) ||
            !sameStorageType(completed(expression.type),
                             completed(module.expressions[choice->falseResult].type)))
          return "conditional changes a function-value signature";
      } else if (const auto *group = std::get_if<ParallelOperation>(&expression.operation)) {
        if (group->result >= id ||
            !sameStorageType(completed(expression.type),
                             completed(module.expressions[group->result].type)))
          return "parallel group changes a returned function signature";
      } else if (const auto *caught = std::get_if<CatchOperation>(&expression.operation)) {
        if (caught->operand >= id || caught->result >= id ||
            !sameStorageType(completed(expression.type),
                             completed(module.expressions[caught->operand].type)) ||
            !sameStorageType(completed(expression.type),
                             completed(module.expressions[caught->result].type)))
          return "catch changes a returned function signature";
      } else if (const auto *match = std::get_if<MatchOperation>(&expression.operation)) {
        for (const auto &arm : match->cases)
          if (arm.result >= id || !sameStorageType(completed(expression.type),
                                                   completed(module.expressions[arm.result].type)))
            return "match changes a returned function signature";
      } else if (const auto *task = std::get_if<TaskOperation>(&expression.operation)) {
        if (task->operand >= id)
          return "task changes a returned function signature";
        const auto &input = module.expressions[task->operand].type;
        auto outputType = completed(expression.type), inputType = completed(input);
        outputType.task = inputType.task = false;
        const bool flags = task->prefix == TaskPrefix::Spawn ? expression.type.task && !input.task
                           : task->prefix == TaskPrefix::Await
                               ? !expression.type.task && input.task && expression.type.fallible
                               : !expression.type.task && !input.task && input.fallible;
        if (!flags || !sameStorageType(outputType, inputType))
          return "task changes a returned function signature";
      } else if (const auto *read = std::get_if<ArrayIndex>(&expression.operation)) {
        if (read->input >= id || expression.type.tensor ||
            !isFunctionArray(module.expressions[read->input].type) ||
            !sameStorageType(expression.type,
                             recordArrayElement(module.expressions[read->input].type)))
          return "array index changes its stored function signature";
      } else if (isFunctionArray(expression.type) &&
                 (std::holds_alternative<TensorLiteral>(expression.operation) ||
                  std::holds_alternative<ArrayMap>(expression.operation) ||
                  std::holds_alternative<ArrayConcat>(expression.operation) ||
                  std::holds_alternative<ArraySlice>(expression.operation) ||
                  std::holds_alternative<ShapeCheck>(expression.operation))) {
        // Element and descriptor invariants are checked by numericCapabilityError.
      } else if (!std::holds_alternative<FunctionReference>(expression.operation) &&
                 !std::holds_alternative<FunctionInvoke>(expression.operation) &&
                 !std::holds_alternative<FunctionCall>(expression.operation) &&
                 !std::holds_alternative<CatchOperation>(expression.operation) &&
                 !std::holds_alternative<MatchOperation>(expression.operation) &&
                 !(std::holds_alternative<FailOperation>(expression.operation) &&
                   expression.type.fallible) &&
                 !(std::get_if<TaskOperation>(&expression.operation) &&
                   std::get<TaskOperation>(expression.operation).prefix == TaskPrefix::Try))
        return "unsupported function-value producer";
    }
    if (const auto *ref = std::get_if<FunctionReference>(&expression.operation)) {
      if (ref->function >= module.functions.size() || !expression.type.callable ||
          expression.type.fallible || expression.type.task || expression.type.tensor ||
          expression.type.array)
        return "function reference has an invalid target or signature";
      const auto &fn = module.functions[ref->function];
      auto result = fn.declaredResult.value_or(fn.resultType);
      // An inferred array length is not a promise in its callable signature.
      if (result.array && result.shape.size() == 1 && expression.type.callable->result.array &&
          expression.type.callable->result.shape.size() == 1)
        result.shape[0] = expression.type.callable->result.shape[0];
      if (fn.captureCount > fn.parameters.size() || ref->captures.size() != fn.captureCount ||
          fn.parameters.size() - fn.captureCount != expression.type.callable->parameters.size() ||
          !sameStorageType(result, expression.type.callable->result))
        return "function reference changes the target signature";
      const auto explicitCount = fn.parameters.size() - fn.captureCount;
      for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        if (fn.parameters[i] >= module.symbols.size())
          return "function reference has invalid parameter metadata";
        if (i < explicitCount) {
          if (!sameStorageType(module.symbols[fn.parameters[i]].type,
                               expression.type.callable->parameters[i]))
            return "function reference changes a target parameter";
        } else {
          const auto capture = ref->captures[i - explicitCount];
          const auto &parameter = module.symbols[fn.parameters[i]].type;
          if (capture >= id || parameter.task || parameter.fallible || parameter.device ||
              parameter.builtin == BuiltinValue::Device ||
              !sameStorageType(parameter, module.expressions[capture].type))
            return "function reference changes a capture type or escapes a scoped value";
        }
      }
    }
    if (const auto *call = std::get_if<FunctionInvoke>(&expression.operation)) {
      if (call->callee >= id)
        return "function-value call has an invalid callee";
      const auto &callee = module.expressions[call->callee].type;
      if (callee.builtin != BuiltinValue::Function || !callee.callable || callee.tensor ||
          callee.array || callee.fallible || callee.task ||
          callee.callable->parameters.size() != call->arguments.size() ||
          !sameFunctionValueType(expression.type, callee.callable->result))
        return "function-value call changes its signature";
      for (std::size_t i = 0; i < call->arguments.size(); ++i)
        if (call->arguments[i] >= id || !([&] {
              auto actual = module.expressions[call->arguments[i]].type;
              const auto &formal = callee.callable->parameters[i];
              if (actual.shape.size() != formal.shape.size())
                return false;
              // Dynamic formal extents accept concrete sizes. Fixed formal
              // extents must already have been checked by semantic conformance.
              for (std::size_t axis = 0; axis < formal.shape.size(); ++axis)
                if (!formal.shape[axis].constant)
                  actual.shape[axis] = formal.shape[axis];
              return sameStorageType(actual, formal);
            })())
          return "function-value call changes an argument type";
    }
    if (const auto *call = std::get_if<FunctionCall>(&expression.operation)) {
      if (call->function >= module.functions.size())
        return "call has an invalid function target";
      const auto &fn = module.functions[call->function];
      if (fn.parameters.size() != call->arguments.size())
        return "call has an invalid argument count";
      if (expression.type.builtin == BuiltinValue::Function &&
          !sameStorageType(expression.type, fn.declaredResult.value_or(fn.resultType)))
        return "call changes a returned function signature";
      for (std::size_t i = 0; i < call->arguments.size(); ++i) {
        if (fn.parameters[i] >= module.symbols.size() || call->arguments[i] >= id)
          return "call has an invalid argument";
        const auto &actual = module.expressions[call->arguments[i]].type;
        const auto &formal = module.symbols[fn.parameters[i]].type;
        if ((actual.builtin == BuiltinValue::Function ||
             formal.builtin == BuiltinValue::Function) &&
            !sameStorageType(actual, formal))
          return "call changes a function parameter signature";
      }
    }
    if (const auto *map = std::get_if<ArrayMap>(&expression.operation)) {
      if (map->function >= module.functions.size())
        return "map has an invalid function target";
      if (map->parallel)
        if (auto error = functionValueTransportError(module, expression))
          return error;
    }
    if (const auto *task = std::get_if<TaskOperation>(&expression.operation);
        task && task->prefix == TaskPrefix::Spawn) {
      if (task->operand >= module.expressions.size())
        return "spawn has an invalid operand";
      const auto &invocation = module.expressions[task->operand];
      if (auto error = functionValueTransportError(module, invocation))
        return error;
    }
  }
  const auto coordinating = continuation::coordinatingFunctions(module);
  const bool resumableReference = std::ranges::any_of(module.expressions, [&](const auto &value) {
    const auto *ref = std::get_if<FunctionReference>(&value.operation);
    return ref && coordinating.at(ref->function);
  });
  if (resumableReference) {
    const auto flow = functionInvocationFlow(module);
    if (!flow)
      return "resumable function-value analysis exceeded its proof budget";
    for (ValueId id = 0; id < module.expressions.size(); ++id) {
      const auto &value = module.expressions[id];
      if (const auto *ref = std::get_if<FunctionReference>(&value.operation);
          ref && flow->reachable[id] && coordinating[ref->function])
        if (auto error = functionValueTransportError(module, value))
          return "resumable callable environments require proven CPU captures: " + *error;
    }
    for (ValueId id = 0; id < module.expressions.size(); ++id)
      if (std::holds_alternative<FunctionInvoke>(module.expressions[id].operation) &&
          std::ranges::any_of(flow->targets[id],
                              [&](auto target) { return coordinating[target]; }) &&
          flow->reachable[id] && flow->unknown[id])
        return "resumable function-value calls require a complete set of proven CPU targets";
    std::vector<unsigned> state(module.functions.size());
    std::size_t remaining = 300000;
    const auto visit = [&](auto &&self, FunctionId id, unsigned depth) -> bool {
      if (!remaining || depth > 128)
        return false;
      if (state[id] == 1 || state[id] == 2)
        return true;
      state[id] = 1;
      for (auto value : module.functions[id].expressions) {
        if (!remaining--)
          return false;
        const auto &op = module.expressions[value].operation;
        std::vector<FunctionId> targets;
        if (const auto *call = std::get_if<FunctionCall>(&op))
          targets.push_back(call->function);
        else if (const auto *map = std::get_if<ArrayMap>(&op))
          targets.push_back(map->function);
        else if (std::holds_alternative<FunctionInvoke>(op))
          targets = flow->targets[value];
        for (auto target : targets)
          if (coordinating[target] && !self(self, target, depth + 1))
            return false;
      }
      state[id] = 2;
      return true;
    };
    for (FunctionId id = 0; id < module.functions.size(); ++id)
      if (coordinating[id] &&
          std::ranges::any_of(module.functions[id].expressions,
                              [&](auto value) { return flow->reachable[value]; }) &&
          !visit(visit, id, 0))
        return "coordinating function-value graph exceeds its proof budget";
  }
  return {};
}
} // namespace kalandra
