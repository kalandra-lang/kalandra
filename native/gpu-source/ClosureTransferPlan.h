#pragma once
#include "AggregateLayout.h"
#include "../lib/ErrorFamilies.h"
#include <expected>
#include <map>
#include <set>

namespace kalandra {
struct ClosureTransferPlan {
  struct Environment {
    FunctionId target;
    // Explicit parameters precede these read-only lexical captures.
    std::vector<SymbolId> captures;
  };
  std::vector<FunctionValueTargets> roots;
  std::vector<Environment> environments;
};

// Plan identities and capture types only. This is not permission to dispatch:
// entry lowering must still validate bodies, native layouts and backend types.
inline std::expected<ClosureTransferPlan, std::string>
planClosureTransfer(const TypedModule &module, std::span<const ValueId> roots,
                    std::size_t budget = 300000) {
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  const auto spend = [&] {
    if (!budget)
      return false;
    --budget;
    return true;
  };
  if (roots.empty() || roots.size() > budget)
    return reject("GPU closure transfer requires source values within the work budget");
  const auto admissible = [&](const ValueType &root) {
    std::vector<ValueType> pending{root};
    std::set<std::size_t> records;
    std::set<const FunctionSignature *> signatures;
    while (!pending.empty()) {
      if (!spend())
        return false;
      const auto type = pending.back();
      pending.pop_back();
      if (type.task || type.fallible || type.errorRecord)
        return false;
      if (deviceScalarValue(type) || deviceFlatArrayValue(type))
        continue;
      if (type.tensor && !type.array && type.builtin == BuiltinValue::Numeric &&
          type.shape.size() <= 8)
        continue;
      if (deviceInlineRecordValue(type) || deviceRecordArrayValue(type)) {
        if (*type.record >= module.records.size())
          return false;
        if (records.insert(*type.record).second) {
          if (module.records[*type.record].fields.size() > budget)
            return false;
          for (const auto &field : module.records[*type.record].fields)
            pending.push_back(field.type);
        }
      } else if (deviceCallableValue(type) && type.callable) {
        if (signatures.insert(type.callable.get()).second) {
          if (type.callable->parameters.size() >= budget)
            return false;
          auto result = type.callable->result;
          if (result.fallible) {
            if (result.task || !result.errorRecord || *result.errorRecord >= module.records.size())
              return false;
            for (auto member : errorMembers(module, *result.errorRecord))
              if (!spend() || member >= module.records.size() ||
                  !module.records[member].error || module.records[member].taskErrorFamily)
                return false;
            ValueType error;
            error.builtin = BuiltinValue::Record;
            error.record = result.errorRecord;
            pending.push_back(error);
            result.fallible = false;
            result.errorRecord.reset();
          }
          pending.push_back(result);
          pending.insert(pending.end(), type.callable->parameters.begin(),
                         type.callable->parameters.end());
        }
      } else
        return false;
    }
    return true;
  };
  std::vector<ValueId> queries(roots.begin(), roots.end());
  std::map<ValueId, std::size_t> queryIndices;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    if (!spend() || queries[i] >= module.expressions.size())
      return reject("GPU closure transfer has an invalid source or exhausted work budget");
    if (!admissible(module.expressions[queries[i]].type))
      return reject("GPU closure source has a host-only type or exceeds the type work budget");
    queryIndices.emplace(queries[i], i);
  }
  std::map<FunctionId, std::vector<ValueId>> references;
  for (ValueId id = 0; id < module.expressions.size(); ++id) {
    if (!spend())
      return reject("GPU closure transfer exceeds the reference work budget");
    if (const auto *ref = std::get_if<FunctionReference>(&module.expressions[id].operation)) {
      if (ref->function >= module.functions.size())
        return reject("GPU closure reference has an invalid target");
      references[ref->function].push_back(id);
      for (auto capture : ref->captures) {
        if (!spend() || capture >= module.expressions.size())
          return reject("GPU closure reference has an invalid capture or exhausted work budget");
        if (containsFunctionStorage(module, module.expressions[capture].type) &&
            !queryIndices.contains(capture)) {
          queryIndices.emplace(capture, queries.size());
          queries.push_back(capture);
        }
      }
    }
  }
  // Reserve a bounded share for provenance; the remaining share is spent on
  // environment discovery rather than resetting the caller's work allowance.
  const auto analysisBudget = budget / 2;
  budget -= analysisBudget;
  const auto flow = functionValueTargets(module, queries, analysisBudget);
  if (!flow)
    return reject("GPU closure provenance is invalid or exceeds the analysis work budget");
  ClosureTransferPlan plan;
  plan.roots.assign(flow->begin(), flow->begin() + roots.size());
  std::set<FunctionId> scheduled;
  std::vector<FunctionId> pending;
  const auto schedule = [&](const FunctionValueTargets &value) {
    if (value.unknown || value.targets.empty())
      return false;
    for (auto target : value.targets) {
      if (!spend())
        return false;
      if (scheduled.insert(target).second)
        pending.push_back(target);
    }
    return true;
  };
  for (const auto &root : plan.roots)
    if (!schedule(root))
      return reject("GPU closure source requires a complete nonempty target set within budget");
  for (std::size_t at = 0; at < pending.size(); ++at) {
    if (!spend())
      return reject("GPU closure environments exceed the work budget");
    const auto target = pending[at];
    const auto &fn = module.functions.at(target);
    if (fn.captureCount > fn.parameters.size() || !references.contains(target))
      return reject("GPU closure target has an invalid environment");
    ClosureTransferPlan::Environment environment{target, {}};
    for (std::size_t i = fn.parameters.size() - fn.captureCount; i < fn.parameters.size(); ++i) {
      const auto symbol = fn.parameters[i];
      if (symbol >= module.symbols.size())
        return reject("GPU closure target '" + fn.name + "' has an invalid capture identity");
      const auto &capture = module.symbols[symbol];
      if (capture.mutableAccess || !admissible(capture.type))
        return reject("GPU closure capture '" + capture.name + "' of type " +
                      describeType(capture.type) + " is not device-compatible; " +
                      "pass supported read-only data or keep this callback on the CPU");
      environment.captures.push_back(symbol);
    }
    for (auto source : references.at(target)) {
      const auto &ref = std::get<FunctionReference>(module.expressions[source].operation);
      if (!spend() || ref.captures.size() != environment.captures.size())
        return reject("GPU closure environment does not match its capture schema or exceeds budget");
      for (std::size_t i = 0; i < ref.captures.size(); ++i) {
        const auto capture = ref.captures[i];
        if (!spend() || !sameFunctionValueType(module.expressions[capture].type,
                                               module.symbols[environment.captures[i]].type))
          return reject("GPU closure capture type does not match its environment or exceeds budget");
        if (containsFunctionStorage(module, module.expressions[capture].type) &&
            !schedule(flow->at(queryIndices.at(capture))))
          return reject("GPU nested closure capture requires complete target provenance");
      }
    }
    plan.environments.push_back(std::move(environment));
  }
  std::ranges::sort(plan.environments, {}, &ClosureTransferPlan::Environment::target);
  return plan;
}
} // namespace kalandra
