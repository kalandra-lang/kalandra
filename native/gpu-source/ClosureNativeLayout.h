#pragma once
#include "ClosureTransferPlan.h"
#include "../lib/TaskValueTypes.h"
#include "llvm/IR/DataLayout.h"

namespace kalandra {
struct NativeClosureLayout {
  struct Capture {
    SymbolId symbol;
    std::uint64_t offset, bytes, alignment;
  };
  FunctionId target;
  std::uint64_t bytes, alignment;
  std::vector<Capture> captures;
};
struct NativeClosureABI {
  std::uint64_t carrierBytes, carrierAlignment, codeOffset, environmentOffset;
  std::vector<NativeClosureLayout> environments;
};

// Matches Backend::closureEnvironment: lexical capture fields are in parameter
// order, with tensors represented by native descriptors. These offsets describe
// host extraction only; native pointer values never become device wire cells.
inline std::expected<NativeClosureABI, std::string>
planNativeClosureLayouts(const TypedModule &module, const ClosureTransferPlan &plan,
                         llvm::LLVMContext &context, const llvm::DataLayout &layout,
                         std::size_t budget = 300000) {
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  if (!budget || plan.environments.size() > budget)
    return reject("native closure layout exceeds its work budget");
  SemanticLimits limits;
  limits.maxValues = budget;
  auto types = TaskValueTypes::create(module, context, limits);
  if (!types)
    return reject(types.error().message);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *carrier = llvm::StructType::get(context, {pointer, pointer});
  NativeClosureABI result{layout.getTypeAllocSize(carrier).getFixedValue(),
                          layout.getABITypeAlign(carrier).value(),
                          layout.getStructLayout(carrier)->getElementOffset(0),
                          layout.getStructLayout(carrier)->getElementOffset(1), {}};
  std::set<FunctionId> seen;
  for (const auto &environment : plan.environments) {
    if (!budget || environment.target >= module.functions.size() ||
        !seen.insert(environment.target).second)
      return reject("native closure layout has an invalid or repeated target, or exceeds budget");
    --budget;
    const auto &fn = module.functions[environment.target];
    if (fn.captureCount > fn.parameters.size() ||
        environment.captures.size() != fn.captureCount || environment.captures.size() > budget)
      return reject("native closure layout does not match the target capture schema");
    budget -= environment.captures.size();
    std::vector<llvm::Type *> fields;
    for (std::size_t i = 0; i < environment.captures.size(); ++i) {
      const auto symbol = environment.captures[i];
      if (symbol >= module.symbols.size() ||
          symbol != fn.parameters[fn.parameters.size() - fn.captureCount + i] ||
          module.symbols[symbol].mutableAccess)
        return reject("native closure layout has an invalid capture identity or permission");
      const auto &type = module.symbols[symbol].type;
      if (type.task || type.fallible || type.errorRecord)
        return reject("native closure capture requires an ordinary value");
      fields.push_back(types->scalar(type));
    }
    auto *storage = llvm::StructType::get(context, fields);
    NativeClosureLayout native{environment.target,
                                layout.getTypeAllocSize(storage).getFixedValue(),
                                layout.getABITypeAlign(storage).value(), {}};
    for (std::size_t i = 0; i < fields.size(); ++i)
      native.captures.push_back({environment.captures[i],
                                 layout.getStructLayout(storage)->getElementOffset(i),
                                 layout.getTypeAllocSize(fields[i]).getFixedValue(),
                                 layout.getABITypeAlign(fields[i]).value()});
    result.environments.push_back(std::move(native));
  }
  return result;
}
} // namespace kalandra
