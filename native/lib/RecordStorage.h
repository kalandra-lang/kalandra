#pragma once
#include "kalandra/Semantics.h"
#include <algorithm>
#include <expected>

namespace kalandra {
// Storage decisions only: this does not admit recursive values to a backend.
enum class RecordFieldStorage { Inline, CollectionDescriptor, ManagedReference };
struct RecordStoragePlan {
  // Canonical component identity is the lowest nominal declaration index.
  std::vector<std::size_t> component;
  std::vector<bool> recursive;
  std::vector<bool> constructible;
  std::vector<std::vector<RecordFieldStorage>> fields;
};
namespace record_storage_detail {
struct Edge {
  std::size_t from, to;
  bool collection;
};
struct Components {
  std::vector<std::size_t> identity;
  std::vector<bool> recursive;
};
inline Components components(std::size_t count, const std::vector<Edge> &edges, bool collections) {
  std::vector<std::vector<std::size_t>> outgoing(count), incoming(count);
  for (const auto &edge : edges)
    if (collections || !edge.collection) {
      outgoing[edge.from].push_back(edge.to);
      incoming[edge.to].push_back(edge.from);
    }
  struct Frame {
    std::size_t id, next = 0;
  };
  std::vector<bool> visited(count);
  std::vector<std::size_t> order;
  for (std::size_t root = 0; root < count; ++root) {
    if (visited[root])
      continue;
    std::vector<Frame> stack{{root}};
    visited[root] = true;
    while (!stack.empty()) {
      auto &frame = stack.back();
      if (frame.next == outgoing[frame.id].size()) {
        order.push_back(frame.id);
        stack.pop_back();
      } else {
        const auto child = outgoing[frame.id][frame.next++];
        if (!visited[child]) {
          visited[child] = true;
          stack.push_back({child});
        }
      }
    }
  }
  Components result;
  result.identity.assign(count, count);
  result.recursive.assign(count, false);
  for (auto next = order.rbegin(); next != order.rend(); ++next) {
    if (result.identity[*next] != count)
      continue;
    std::vector<std::size_t> stack{*next}, members;
    result.identity[*next] = *next;
    auto canonical = *next;
    while (!stack.empty()) {
      const auto id = stack.back();
      stack.pop_back();
      members.push_back(id);
      canonical = std::min(canonical, id);
      for (auto child : incoming[id])
        if (result.identity[child] == count) {
          result.identity[child] = *next;
          stack.push_back(child);
        }
    }
    for (auto id : members) {
      result.identity[id] = canonical;
      result.recursive[id] = members.size() > 1;
    }
  }
  for (const auto &edge : edges)
    if ((collections || !edge.collection) && edge.from == edge.to)
      result.recursive[edge.from] = true;
  return result;
}
} // namespace record_storage_detail
// Bounded structural planning after nominal identities are available. Full typed-IR
// validation remains required separately. Budget counts types, fields, cases and slots.
inline std::expected<RecordStoragePlan, Diagnostic> planRecordStorage(const TypedModule &module,
                                                                      std::size_t budget = 300000) {
  const auto fail = [](std::string message, SourceSpan span = {}) {
    return std::unexpected(Diagnostic{"S068", std::move(message), span});
  };
  const auto charge = [&](std::size_t count) {
    if (count > budget)
      return false;
    budget -= count;
    return true;
  };
  if (!charge(module.records.size()))
    return fail("record storage graph exceeds configured work budget");
  std::vector<record_storage_detail::Edge> edges;
  RecordStoragePlan plan;
  plan.fields.resize(module.records.size());
  for (std::size_t id = 0; id < module.records.size(); ++id) {
    const auto &record = module.records[id];
    if (!charge(record.fields.size()) || !charge(record.variants.size()))
      return fail("record storage graph exceeds configured work budget", record.declaration);
    if (record.fields.empty())
      return fail("record storage graph requires populated declarations", record.declaration);
    std::vector<bool> assigned(record.fields.size());
    if (!record.variants.empty()) {
      assigned[0] = true; // The checked enum discriminator is not a payload dependency.
      for (const auto &variant : record.variants) {
        if (!charge(variant.fields.size()))
          return fail("record storage graph exceeds configured work budget", variant.declaration);
        for (auto field : variant.fields) {
          if (!field || field >= assigned.size() || assigned[field])
            return fail("invalid record storage variant payload slot", variant.declaration);
          assigned[field] = true;
        }
      }
      if (std::ranges::find(assigned, false) != assigned.end())
        return fail("record storage variant has unassigned payload fields", record.declaration);
    }
    for (const auto &field : record.fields) {
      const auto &type = field.type;
      plan.fields[id].push_back(type.tensor ? RecordFieldStorage::CollectionDescriptor
                                            : RecordFieldStorage::Inline);
      if (type.builtin != BuiltinValue::Record) {
        if (type.record)
          return fail("non-record storage field carries nominal identity", field.declaration);
        continue; // Function signatures are not stored record values.
      }
      if (!type.record || *type.record >= module.records.size() ||
          (type.array ? (!type.tensor || type.shape.size() != 1) : type.tensor))
        return fail("invalid nominal record storage reference", field.declaration);
      edges.push_back({id, *type.record, type.array});
    }
  }
  auto all = record_storage_detail::components(module.records.size(), edges, true);
  const auto inlineOnly = record_storage_detail::components(module.records.size(), edges, false);
  plan.component = std::move(all.identity);
  plan.recursive = std::move(all.recursive);
  for (std::size_t id = 0; id < module.records.size(); ++id)
    for (std::size_t field = 0; field < module.records[id].fields.size(); ++field) {
      const auto &type = module.records[id].fields[field].type;
      if (type.record && !type.array && inlineOnly.recursive[id] &&
          inlineOnly.identity[id] == inlineOnly.identity[*type.record])
        plan.fields[id][field] = RecordFieldStorage::ManagedReference;
    }
  if (std::ranges::any_of(module.records,
                          [](const auto &record) { return !record.errorOrigins.empty(); })) {
    // Error unions forward nominal fields from their original variants. Keep
    // each forwarded slot's representation identical in every carrier: boxing
    // can be required by either the original family or a recursive union user.
    std::vector<std::size_t> offsets;
    std::vector<std::pair<std::size_t, std::size_t>> slots;
    for (std::size_t id = 0; id < module.records.size(); ++id) {
      offsets.push_back(slots.size());
      for (std::size_t field = 0; field < module.records[id].fields.size(); ++field)
        slots.push_back({id, field});
    }
    std::vector<std::vector<std::size_t>> equivalents(slots.size());
    for (std::size_t id = 0; id < module.records.size(); ++id) {
      const auto &record = module.records[id];
      if (record.errorOrigins.empty())
        continue;
      if (record.errorOrigins.size() != record.variants.size())
        return fail("invalid error storage variant origins", record.declaration);
      for (std::size_t variant = 0; variant < record.variants.size(); ++variant) {
        const auto [member, original] = record.errorOrigins[variant];
        if (member >= module.records.size() || original >= module.records[member].variants.size())
          return fail("invalid error storage variant identity", record.declaration);
        const auto &from = module.records[member].variants[original].fields;
        const auto &to = record.variants[variant].fields;
        if (from.size() != to.size() || !charge(to.size()))
          return fail("invalid or over-budget error storage forwarding", record.declaration);
        for (std::size_t n = 0; n < to.size(); ++n) {
          const auto &a = module.records[member].fields[from[n]].type;
          const auto &b = record.fields[to[n]].type;
          if (!a.record || a.tensor)
            continue;
          if (a.record != b.record || b.tensor)
            return fail("inconsistent nominal error storage forwarding", record.declaration);
          const auto x = offsets[member] + from[n], y = offsets[id] + to[n];
          equivalents[x].push_back(y);
          equivalents[y].push_back(x);
        }
      }
    }
    std::vector<std::size_t> pending;
    std::vector<bool> visited(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
      const auto [id, field] = slots[i];
      if (plan.fields[id][field] == RecordFieldStorage::ManagedReference)
        pending.push_back(i);
    }
    while (!pending.empty()) {
      const auto next = pending.back();
      pending.pop_back();
      if (visited[next])
        continue;
      visited[next] = true;
      const auto [id, field] = slots[next];
      plan.fields[id][field] = RecordFieldStorage::ManagedReference;
      for (auto other : equivalents[next])
        pending.push_back(other);
    }
  }
  // A record needs every field; an enum needs one finite variant. Empty arrays
  // terminate construction, so their element types are not prerequisites.
  struct Clause {
    std::size_t owner, remaining = 0;
  };
  std::vector<Clause> clauses;
  std::vector<std::vector<std::size_t>> users(module.records.size());
  for (std::size_t id = 0; id < module.records.size(); ++id) {
    const auto &record = module.records[id];
    const auto addClause = [&](const std::vector<std::size_t> &fields) {
      const auto clause = clauses.size();
      clauses.push_back({id});
      for (auto field : fields) {
        const auto &type = record.fields[field].type;
        if (type.record && !type.array) {
          ++clauses[clause].remaining;
          users[*type.record].push_back(clause);
        }
      }
    };
    if (record.variants.empty()) {
      std::vector<std::size_t> fields;
      for (std::size_t field = 0; field < record.fields.size(); ++field)
        fields.push_back(field);
      addClause(fields);
    } else
      for (const auto &variant : record.variants)
        addClause(variant.fields);
  }
  plan.constructible.assign(module.records.size(), false);
  std::vector<std::size_t> ready;
  const auto mark = [&](std::size_t id) {
    if (!plan.constructible[id]) {
      plan.constructible[id] = true;
      ready.push_back(id);
    }
  };
  for (const auto &clause : clauses)
    if (!clause.remaining)
      mark(clause.owner);
  for (std::size_t at = 0; at < ready.size(); ++at)
    for (auto clause : users[ready[at]])
      if (--clauses[clause].remaining == 0)
        mark(clauses[clause].owner);
  return plan;
}
} // namespace kalandra
