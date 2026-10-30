#pragma once
#include "kalandra/CollectionABI.h"
#include "kalandra/Semantics.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include <stdexcept>

namespace kalandra {
inline void collectionMetadata(llvm::Module &module, const ValueType &type) {
  if (!type.tensor ||
      (type.builtin != BuiltinValue::Numeric && !isBoolArray(type) && !isStringArray(type)) ||
      type.device || type.shape.size() > cli::maximumCollectionRank ||
      (type.array && type.shape.size() != 1))
    throw std::runtime_error(
        "collection CLI entry requires a CPU numeric array/tensor within the frontend rank limit");
  auto *integer = llvm::Type::getInt64Ty(module.getContext());
  const auto field = [&](const char *name, std::uint64_t value) {
    new llvm::GlobalVariable(module, integer, true, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantInt::get(integer, value), name);
  };
  field("kalandra_collection_rank_v1", type.shape.size());
  field("kalandra_collection_element_v1", isStringArray(type) ? cli::stringArrayElement
                                          : isBoolArray(type)
                                              ? cli::boolArrayElement
                                              : static_cast<unsigned>(type.element));
  field("kalandra_collection_array_v1", type.array);
}
} // namespace kalandra
