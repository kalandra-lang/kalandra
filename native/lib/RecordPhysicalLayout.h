#pragma once
#include "RecordLayout.h"
#include "RecordStorage.h"

namespace kalandra {
struct RecordPhysicalLayout {
  RecordStoragePlan storage;
  // Child layouts precede their inline users; references/descriptors are leaves.
  std::vector<std::size_t> order;
  // Scalar/pointer units bound expansion; these are not target ABI byte sizes.
  std::vector<std::size_t> units;
};
inline std::expected<RecordPhysicalLayout, Diagnostic>
planRecordPhysicalLayout(const TypedModule &module, const SemanticLimits &limits = {}) {
  auto storage = planRecordStorage(module);
  if (!storage)
    return std::unexpected(storage.error());
  if (auto invalid = recordFieldError(module))
    return std::unexpected(*invalid);
  RecordPhysicalLayout layout{std::move(*storage), {}, {}};
  const auto count = module.records.size();
  std::vector<std::vector<std::size_t>> users(count);
  std::vector<std::size_t> remaining(count), depth(count, 1);
  layout.units.resize(count);
  const auto fail = [&](std::size_t id, std::string message) {
    return std::unexpected(Diagnostic{"S068", std::move(message), module.records[id].declaration});
  };
  for (std::size_t id = 0; id < count; ++id) {
    for (std::size_t field = 0; field < module.records[id].fields.size(); ++field) {
      const auto &type = module.records[id].fields[field].type;
      const auto kind = layout.storage.fields[id][field];
      if (kind == RecordFieldStorage::Inline && type.record) {
        ++remaining[id];
        users[*type.record].push_back(id);
        continue;
      }
      const auto units = kind == RecordFieldStorage::ManagedReference ? 1
                         : type.tensor                                ? 3 + 2 * type.shape.size()
                         : type.builtin == BuiltinValue::TaskError    ? 4
                         : type.builtin == BuiltinValue::Function     ? 2
                         : type.builtin == BuiltinValue::String       ? 3
                                                                      : 1;
      if (units > limits.maxValues - layout.units[id])
        return fail(id, "record physical layout exceeds configured scalar-field budget");
      layout.units[id] += units;
    }
    if (!remaining[id])
      layout.order.push_back(id);
  }
  for (std::size_t at = 0; at < layout.order.size(); ++at) {
    const auto child = layout.order[at];
    if (depth[child] > limits.maxDepth)
      return fail(child, "record physical layout exceeds configured nesting depth");
    for (auto parent : users[child]) {
      if (layout.units[child] > limits.maxValues - layout.units[parent])
        return fail(parent, "record physical layout exceeds configured scalar-field budget");
      layout.units[parent] += layout.units[child];
      depth[parent] = std::max(depth[parent], depth[child] + 1);
      if (!--remaining[parent])
        layout.order.push_back(parent);
    }
  }
  if (layout.order.size() != count)
    return std::unexpected(
        Diagnostic{"S068", "record physical layout has an unbroken inline cycle", {}});
  return layout;
}
} // namespace kalandra
