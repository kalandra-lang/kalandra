#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
// Functionalize scalar loop exits into local state so the synchronous backend
// can compile a fused traversal as one checked native loop. Atomic on failure.
void normalizeFusedScalarControl(TypedModule &, FunctionId);
} // namespace kalandra
