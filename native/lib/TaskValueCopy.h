#pragma once
#include "kalandra/Semantics.h"
#include <span>
#include <vector>
namespace llvm {
class GlobalVariable;
class Module;
} // namespace llvm
namespace kalandra {
// Private compiler component entry: emit the production task value layout and
// its copy callbacks without source/continuation admission or task entry setup.
// Only non-callable storage is supported here; callable layouts require task
// entry linkage. The module must already use the checked invocation ABI.
llvm::GlobalVariable *emitTaskValueLayout(const TypedModule &, llvm::Module &, const ValueType &);
// Component linkage for complete callable/aggregate copy emission. The first
// module supplies only nominal schemas; kernels supply checked callable target
// metadata whose kalandra_callable_N entries already exist in the LLVM module.
// Both modules must use the same nominal identities. No task entries are emitted.
llvm::GlobalVariable *emitTaskKernelValueLayout(const TypedModule &, const TypedModule &kernels,
                                                llvm::Module &, const ValueType &);
// Compiler frame storage can carry a stopped result before its error edge runs.
// This layout skips absent success payloads without weakening packet validation.
llvm::GlobalVariable *emitTaskFrameValueLayout(const TypedModule &, const TypedModule &kernels,
                                               llvm::Module &, const ValueType &);
// Completed native payloads (without the task Failure wrapper), for opaque
// compiler-owned host slots. The schema is nominal metadata only; callable
// storage requires linked callable entries and is rejected by this component.
std::vector<llvm::GlobalVariable *> emitTaskPayloadValueLayouts(const TypedModule &, llvm::Module &,
                                                                std::span<const ValueType>);
} // namespace kalandra
