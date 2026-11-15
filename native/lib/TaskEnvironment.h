#pragma once
#include "kalandra/Semantics.h"
#include <map>
#include <span>
namespace llvm {
class Function;
class Module;
} // namespace llvm
namespace kalandra {
// Private checked-plan support. Reuse recursive graph boxing for immutable
// capture payloads; the caller binds each published environment to its entry.
std::map<std::size_t, llvm::Function *>
emitTaskEnvironmentBoxes(const TypedModule &, std::span<const std::size_t>, llvm::Module &);
// Box nominal item payloads with the same dependency graph and invocation ABI
// used by ordinary record constructors.
std::map<std::size_t, llvm::Function *>
emitTaskRecordBoxes(const TypedModule &, std::span<const std::size_t>, llvm::Module &);
} // namespace kalandra
