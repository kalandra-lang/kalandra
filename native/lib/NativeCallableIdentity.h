#pragma once
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Module.h"
#include <string>
namespace kalandra {
// Continuation lowering imports synchronous subgraphs into a new function
// namespace. Component copy adapters must resolve the original checked identity,
// rather than infer a projection's ordinal or backend from its return type.
inline llvm::Function *sourceCallableCode(llvm::Module &module, std::size_t id, bool fallible) {
  const auto name = "kalandra_source_callable_" + std::to_string(id);
  if (auto *function = module.getFunction(name))
    return function;
  if (auto *alias = module.getNamedAlias(name))
    return llvm::dyn_cast<llvm::Function>(alias->getAliaseeObject());
  return module.getFunction(
      std::string(fallible ? "kalandra_callable_plan_" : "kalandra_callable_") +
      std::to_string(id));
}
} // namespace kalandra
