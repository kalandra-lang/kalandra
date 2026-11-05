#pragma once
#include "../lib/TaskValueTypes.h"
#include "AggregateLayout.h"
#include "llvm/IR/DataLayout.h"

namespace kalandra {
struct AggregateFieldABI {
  std::uint64_t nativeOffset, nativeBytes, nativeAlignment;
  std::size_t deviceOffset, deviceWords, activeTag;
  RecordFieldStorage storage;
};
struct AggregateABI {
  std::uint64_t nativeBytes, nativeAlignment;
  std::size_t deviceWords;
  std::vector<AggregateFieldABI> fields;
};
struct NativeAggregateFieldABI {
  std::uint64_t nativeOffset, nativeBytes, nativeAlignment;
  std::size_t activeTag;
  RecordFieldStorage storage;
};
struct NativeAggregateABI {
  std::uint64_t nativeBytes, nativeAlignment;
  std::vector<NativeAggregateFieldABI> fields;
};
// Native slots may hold CPU-only fields while coordinating device work. Their
// physical ABI must not depend on whether those fields have a device encoding.
inline std::expected<std::vector<NativeAggregateABI>, std::string>
planNativeAggregateLayouts(const TypedModule &input, llvm::LLVMContext &context,
                           const llvm::DataLayout &layout, const SemanticLimits &limits = {}) {
  auto native = TaskValueTypes::create(input, context, limits);
  if (!native)
    return std::unexpected(native.error().message);
  std::vector<NativeAggregateABI> result;
  for (std::size_t id = 0; id < input.records.size(); ++id) {
    auto *type = native->records()[id];
    NativeAggregateABI record{
        layout.getTypeAllocSize(type).getFixedValue(), layout.getABITypeAlign(type).value(), {}};
    for (std::size_t field = 0; field < input.records[id].fields.size(); ++field) {
      auto *element = type->getElementType(field);
      record.fields.push_back({layout.getStructLayout(type)->getElementOffset(field),
                               layout.getTypeAllocSize(element).getFixedValue(),
                               layout.getABITypeAlign(element).value(), native->fieldTag(id, field),
                               native->fieldStorage(id, field)});
    }
    result.push_back(std::move(record));
  }
  return result;
}
// Target-native addresses are described separately from canonical device cells.
// Consumers traverse checked fields and active enum payloads; native padding,
// pointers and inactive payload bytes are never a wire representation. This
// metadata does not admit a source aggregate or supply ownership callbacks.
inline std::expected<std::map<std::size_t, AggregateABI>, std::string>
planAggregateABI(const TypedModule &input, llvm::LLVMContext &context,
                 const llvm::DataLayout &nativeLayout, std::span<const ValueType> roots,
                 std::size_t budget = 300000, const RecordStoragePlan *storage = nullptr) {
  auto device = planDeviceAggregateLayouts(input, roots, budget, storage);
  if (!device)
    return std::unexpected(device.error());
  SemanticLimits limits;
  limits.maxValues = budget;
  auto native = planNativeAggregateLayouts(input, context, nativeLayout, limits);
  if (!native)
    return std::unexpected(native.error());
  std::map<std::size_t, AggregateABI> result;
  for (const auto &[id, cells] : *device) {
    const auto &sourceABI = native->at(id);
    AggregateABI record{sourceABI.nativeBytes, sourceABI.nativeAlignment, cells.words, {}};
    for (std::size_t field = 0; field < cells.fields.size(); ++field) {
      const auto &nativeField = sourceABI.fields[field];
      const auto &source = input.records[id].fields[field].type;
      const auto width = deviceInlineRecordValue(source) && !cells.boxed[field]
                             ? device->at(*source.record).words : 1;
      record.fields.push_back({nativeField.nativeOffset, nativeField.nativeBytes,
                               nativeField.nativeAlignment, cells.fields[field], width,
                               nativeField.activeTag, nativeField.storage});
    }
    result.emplace(id, std::move(record));
  }
  return result;
}
} // namespace kalandra
