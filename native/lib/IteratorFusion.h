#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
// Run only on fully checked IR. Failure to prove a local, scalar, single-pass
// producer leaves its independently owned generator implementation unchanged.
void fuseLocalIterators(TypedModule &);
// Internal region extraction uses the same proof; a failed proof is atomic.
bool fuseLocalIteratorFunction(TypedModule &, FunctionId);
void extractLocalIteratorRegions(TypedModule &);
} // namespace kalandra
