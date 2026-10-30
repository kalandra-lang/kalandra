#pragma once
#include "ErrorFamilies.h"
#include "RecordStorage.h"
#include "kalandra/Semantics.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace kalandra {
inline bool hasRecordBacking(const TypedModule &module) {
  for (const auto &expression : module.expressions)
    if ((std::get_if<FunctionReference>(&expression.operation) &&
         !std::get<FunctionReference>(expression.operation).captures.empty()) ||
        isOwnedArray(expression.type) ||
        (expression.type.callable &&
         (expression.type.callable->result.tensor ||
          std::ranges::any_of(expression.type.callable->parameters,
                              [](const auto &type) { return type.tensor; }))))
      return true;
  for (const auto &symbol : module.symbols)
    if (isOwnedArray(symbol.type))
      return true;
  for (const auto &record : module.records)
    for (const auto &field : record.fields)
      if (field.type.tensor || field.type.builtin == BuiltinValue::String)
        return true;
  if (auto storage = planRecordStorage(module))
    for (const auto &fields : storage->fields)
      if (std::ranges::find(fields, RecordFieldStorage::ManagedReference) != fields.end())
        return true;
  return false;
}
// Validate completed field metadata independently of nominal dependency cycles.
inline std::optional<Diagnostic> recordFieldError(const TypedModule &module) {
  const auto fail = [](std::string message, SourceSpan span) {
    return std::optional<Diagnostic>{{"S068", std::move(message), span}};
  };
  bool sawTaskError = false;
  for (const auto &record : module.records) {
    if (record.name == "TaskError" && !record.taskErrorFamily)
      return fail("reserved TaskError identity requires its canonical family metadata",
                  record.declaration);
    if (record.taskErrorFamily) {
      if (sawTaskError || !record.error || record.name != "TaskError" ||
          !record.errorMembers.empty() || !record.errorOrigins.empty() ||
          record.fields.size() != 5 || record.variants.size() != 4)
        return fail("invalid canonical TaskError family", record.declaration);
      sawTaskError = true;
      const std::vector<std::string> labels{"Fault", "Cancelled", "DeadlineExceeded",
                                            "AdmissionRejected"};
      for (std::size_t i = 0; i < 4; ++i)
        if (record.variants[i].name != labels[i] ||
            record.variants[i].fields != std::vector<std::size_t>{i + 1} ||
            record.fields[i + 1].type.builtin != BuiltinValue::TaskError)
          return fail("invalid canonical TaskError variant", record.declaration);
    }
    if (!record.errorMembers.empty()) {
      if (!record.error || record.errorMembers.size() < 2 ||
          !std::ranges::is_sorted(record.errorMembers) ||
          std::adjacent_find(record.errorMembers.begin(), record.errorMembers.end()) !=
              record.errorMembers.end() ||
          record.errorOrigins.size() != record.variants.size())
        return fail("invalid error-union family metadata", record.declaration);
      std::size_t count = 0;
      for (auto member : record.errorMembers) {
        if (member >= module.records.size() || !module.records[member].error ||
            !module.records[member].errorMembers.empty())
          return fail("invalid error-union member", record.declaration);
        const auto &original = module.records[member];
        for (std::size_t variant = 0; variant < original.variants.size(); ++variant, ++count) {
          if (count >= record.errorOrigins.size() ||
              record.errorOrigins[count] != std::pair{member, variant} ||
              record.variants[count].fields.size() != original.variants[variant].fields.size())
            return fail("invalid error-union variant identity", record.declaration);
          for (std::size_t i = 0; i < original.variants[variant].fields.size(); ++i) {
            auto a = record.variants[count].fields[i], b = original.variants[variant].fields[i];
            if (a >= record.fields.size() || b >= original.fields.size())
              return fail("invalid error-union payload slot", record.declaration);
            const auto &x = record.fields[a], &y = original.fields[b];
            if (x.name != y.name || x.type.builtin != y.type.builtin ||
                x.type.record != y.type.record || x.type.tensor != y.type.tensor ||
                x.type.array != y.type.array || x.type.element != y.type.element ||
                x.type.shape.size() != y.type.shape.size() ||
                (x.type.builtin == BuiltinValue::Function &&
                 !sameFunctionValueType(x.type, y.type)))
              return fail("invalid error-union payload type", record.declaration);
            for (std::size_t axis = 0; axis < x.type.shape.size(); ++axis)
              if (x.type.shape[axis].constant != y.type.shape[axis].constant)
                return fail("invalid error-union payload extent", record.declaration);
          }
        }
      }
      if (count != record.variants.size())
        return fail("invalid error-union variant count", record.declaration);
    } else if (!record.errorOrigins.empty())
      return fail("non-union type carries error origins", record.declaration);
    if (record.fields.empty())
      return fail("record declarations require fields", record.declaration);
    if (record.error && record.variants.empty())
      return fail("error declarations require variants", record.declaration);
    if (!record.variants.empty()) {
      const auto &tag = record.fields.front().type;
      if (tag.builtin != BuiltinValue::Numeric || tag.element != NumericType::Int64 || tag.tensor ||
          tag.array || tag.task || tag.fallible || tag.device)
        return fail("invalid enum discriminator layout", record.declaration);
      std::unordered_set<std::string> names;
      std::vector<bool> used(record.fields.size());
      used[0] = true;
      for (const auto &variant : record.variants) {
        if (variant.name.empty() || !names.insert(variant.name).second)
          return fail("invalid or duplicate enum variant identity", variant.declaration);
        for (auto slot : variant.fields) {
          if (!slot || slot >= used.size() || used[slot])
            return fail("invalid enum payload field layout", variant.declaration);
          used[slot] = true;
        }
      }
      if (std::ranges::find(used, false) != used.end())
        return fail("enum layout has an unassigned payload slot", record.declaration);
    }
    for (const auto &field : record.fields) {
      const auto &type = field.type;
      if (type.task || type.fallible || type.errorRecord || type.device ||
          (type.tensor
               ? ((type.builtin != BuiltinValue::Numeric && !isBoolArray(type) &&
                   !isStringArray(type) && !isRecordArray(type) && !isFunctionArray(type)) ||
                  type.shape.size() > 128 || (type.array && type.shape.size() != 1))
               : (type.array || !type.shape.empty())))
        return fail(
            "record fields require completed CPU numbers, numeric collections, Bool, Bool "
            "arrays, Strings, String arrays, TaskError values, functions, records or record arrays",
            field.declaration);
      if (type.builtin == BuiltinValue::Record) {
        if (!type.record || *type.record >= module.records.size())
          return fail("invalid nested record declaration identity", field.declaration);
      } else if (type.record ||
                 (type.builtin != BuiltinValue::Bool && type.builtin != BuiltinValue::String &&
                  type.builtin != BuiltinValue::TaskError &&
                  type.builtin != BuiltinValue::Function &&
                  (type.builtin != BuiltinValue::Numeric ||
                   !(type.array
                         ? numericCollectionStorageAvailable(type.element, NumericTarget::Cpu)
                     : type.tensor
                         ? numericTensorElementwiseAvailable(type.element, NumericTarget::Cpu)
                         : numericScalarArithmeticAvailable(type.element, NumericTarget::Cpu)))))
        return fail("record fields require CPU numbers, numeric collections, Bool, Bool arrays, "
                    "Strings, String arrays, TaskError values, functions, records or record arrays",
                    field.declaration);
    }
  }
  return {};
}
// Execution gate: ownership and transport walkers still require acyclic types,
// including array-element dependencies. Storage planning does not open this gate.
inline std::optional<Diagnostic> recordLayoutError(const TypedModule &module,
                                                   const SemanticLimits &limits = {}) {
  if (auto invalid = recordFieldError(module))
    return invalid;
  const auto fail = [](std::string message, SourceSpan span) {
    return std::optional<Diagnostic>{{"S068", std::move(message), span}};
  };
  struct Frame {
    std::size_t record, next = 0;
  };
  std::vector<unsigned> state(module.records.size());
  std::vector<std::size_t> heights(module.records.size(), 1), leaves(module.records.size());
  for (std::size_t root = 0; root < module.records.size(); ++root) {
    if (state[root] == 2)
      continue;
    std::vector<Frame> stack{{root}};
    state[root] = 1;
    while (!stack.empty()) {
      auto &frame = stack.back();
      const auto id = frame.record;
      const auto &record = module.records[id];
      if (frame.next == record.fields.size()) {
        if (heights[id] > limits.maxDepth)
          return fail("record layout exceeds configured nesting depth", record.declaration);
        state[id] = 2;
        stack.pop_back();
        continue;
      }
      const auto &field = record.fields[frame.next];
      std::size_t count = field.type.tensor ? 3 + 2 * field.type.shape.size()
                          : field.type.builtin == BuiltinValue::TaskError ? 4
                          : field.type.builtin == BuiltinValue::Function  ? 2
                          : field.type.builtin == BuiltinValue::String    ? 3
                                                                          : 1;
      if (field.type.record) {
        const auto child = *field.type.record;
        if (state[child] == 1)
          return fail("recursive record/enum field type cycles are not yet supported",
                      field.declaration);
        if (!state[child]) {
          // Also bound the explicit DFS stack before completing a long chain.
          if (stack.size() >= limits.maxDepth)
            return fail("record layout exceeds configured nesting depth", field.declaration);
          state[child] = 1;
          stack.push_back({child});
          continue;
        }
        heights[id] = std::max(heights[id], heights[child] + 1);
        if (!field.type.tensor)
          count = leaves[child];
      }
      if (count > limits.maxValues - leaves[id])
        return fail("record layout exceeds configured expanded scalar-field budget",
                    field.declaration);
      leaves[id] += count;
      ++frame.next;
    }
  }
  return {};
}
// Count finite physical storage iteratively, treating managed references and
// collection descriptors as leaves. Callers still validate complete layouts.
inline std::size_t recordInlineUnits(const TypedModule &module, std::size_t root) {
  auto storage = planRecordStorage(module);
  constexpr auto cap = std::numeric_limits<std::size_t>::max();
  if (!storage || root >= module.records.size())
    return cap;
  struct Frame {
    std::size_t id, field = 0, units = 0;
  };
  std::vector<std::optional<std::size_t>> cache(module.records.size());
  std::vector<Frame> pending{{root}};
  while (!pending.empty()) {
    auto &frame = pending.back();
    if (frame.field == module.records[frame.id].fields.size()) {
      cache[frame.id] = std::max<std::size_t>(1, frame.units);
      pending.pop_back();
      continue;
    }
    const auto &type = module.records[frame.id].fields[frame.field].type;
    const auto managed =
        storage->fields[frame.id][frame.field] == RecordFieldStorage::ManagedReference;
    if (!managed && !type.tensor && type.record && !cache[*type.record]) {
      pending.push_back({*type.record});
      continue;
    }
    const auto units = managed                                  ? 1
                       : type.tensor                            ? 3 + 2 * type.shape.size()
                       : type.record                            ? *cache[*type.record]
                       : type.builtin == BuiltinValue::Function ? 2
                       : type.builtin == BuiltinValue::String   ? 3
                                                                : 1;
    if (units > cap - frame.units)
      return cap;
    frame.units += units;
    ++frame.field;
  }
  return *cache[root];
}

} // namespace kalandra
