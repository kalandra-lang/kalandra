#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
// Source access proof, separate from lifetime/ownership lowering. Unknown origins
// cannot grant write access. No execution or cancellation contract changes here.
[[nodiscard]] std::vector<Diagnostic> arrayAccessDiagnostics(const TypedModule &);
} // namespace kalandra
