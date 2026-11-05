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
// Target-native addresses are described separately from canonical device cells.
// Consumers traverse checked fields and active enum payloads; native padding,
// pointers and inactive payload bytes are never a wire representation. This
// metadata does not admit a source aggregate or supply ownership callbacks.
inline std::expected<std::map<std::size_t, AggregateABI>, std::string>
planAggregateABI(const TypedModule &input, llvm::LLVMContext &context,
                 const llvm::DataLayout &nativeLayout, std::span<const ValueType> roots,
                 std::size_t budget = 300000) {
  auto device = planDeviceAggregateLayouts(input, roots, budget);
  if (!device)
    return std::unexpected(device.error());
  SemanticLimits limits;
  limits.maxValues = budget;
  auto native = TaskValueTypes::create(input, context, limits);
  if (!native)
    return std::unexpected(native.error().message);
  std::map<std::size_t, AggregateABI> result;
  for (const auto &[id, cells] : *device) {
    auto *type = native->records()[id];
    auto *layout = nativeLayout.getStructLayout(type);
    AggregateABI record{nativeLayout.getTypeAllocSize(type).getFixedValue(),
                        nativeLayout.getABITypeAlign(type).value(),
                        cells.words,
                        {}};
    for (std::size_t field = 0; field < cells.fields.size(); ++field) {
      auto *element = type->getElementType(field);
      const auto &source = input.records[id].fields[field].type;
      const auto width = deviceInlineRecordValue(source) ? device->at(*source.record).words : 1;
      record.fields.push_back({layout->getElementOffset(field),
                               nativeLayout.getTypeAllocSize(element).getFixedValue(),
                               nativeLayout.getABITypeAlign(element).value(), cells.fields[field],
                               width, cells.fieldTags[field], native->fieldStorage(id, field)});
    }
    result.emplace(id, std::move(record));
  }
  return result;
}
} // namespace kalandra
