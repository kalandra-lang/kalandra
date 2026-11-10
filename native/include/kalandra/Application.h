#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
struct ApplicationEntryInfo {
  FunctionId function;
  bool arguments, unit;
};
// CPU and mixed-device frontends share the same source entry contract.
[[nodiscard]] std::expected<ApplicationEntryInfo, Diagnostic>
validateApplicationEntry(const TypedModule &);
// Validate the source application's entry and add a small, ordinary checked
// wrapper with a fixed i32 exit-code ABI. Library/test lowering does not call
// this pass; its numeric observation and explicit exports remain independent.
[[nodiscard]] std::optional<Diagnostic> prepareApplication(TypedModule &,
                                                           SemanticLimits limits = {});
} // namespace kalandra
