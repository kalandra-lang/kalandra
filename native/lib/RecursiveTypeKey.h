#pragma once
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include <stdexcept>
namespace kalandra {
inline llvm::GlobalVariable *recursiveTypeKey(llvm::Module &module, std::size_t id) {
  const auto name = "kalandra_recursive_type_" + std::to_string(id);
  auto *found = module.getNamedGlobal(name);
  if (found) {
    if (!found->getValueType()->isIntegerTy(8) || !found->isConstant() ||
        !found->hasLocalLinkage() || !found->hasInitializer() ||
        found->getUnnamedAddr() != llvm::GlobalValue::UnnamedAddr::None)
      throw std::invalid_argument("invalid recursive nominal token");
    return found;
  }
  return new llvm::GlobalVariable(
      module, llvm::Type::getInt8Ty(module.getContext()), true, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(module.getContext()), 0), name);
}
} // namespace kalandra
