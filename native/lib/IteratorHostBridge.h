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
// Emit a component's cursor bridges together so item/error layout identities
// remain shared between construction and advancement.
void emitIteratorHostBridges(const TypedModule &, llvm::Module &,
                             std::span<const IteratorBridgeRequest>);
} // namespace kalandra
