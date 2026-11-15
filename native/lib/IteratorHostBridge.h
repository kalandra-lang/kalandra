#pragma once
#include "kalandra/Semantics.h"
#include <span>
namespace llvm {
class Module;
}
namespace kalandra {
struct IteratorBridgeRequest {
  std::string name;
  IteratorOperation operation;
  ValueType result;
};
struct GeneratorBridgeRequest {
  std::string name;
  FunctionId producer;
};
// Emit a component's cursor bridges together so item/error layout identities
// remain shared between construction and advancement. Bridges receive both the
// active Invocation and its Task, and return false when the supervisor must
// retry the same instruction after suspension or handle an execution failure.
void emitIteratorHostBridges(const TypedModule &, llvm::Module &,
                             std::span<const IteratorBridgeRequest>,
                             std::span<const GeneratorBridgeRequest>, const TypedModule &kernels,
                             std::span<const FunctionId> hostTargets,
                             std::span<const std::pair<FunctionId, FunctionId>> hostNativeTargets);
} // namespace kalandra
