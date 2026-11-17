#pragma once
#include "kalandra/NumericType.h"
#include <cstdint>

namespace kalandra {
enum class WorkgroupAction { Dispatch, GlobalIndex, LocalIndex, Shared, Barrier, Read, Write };
// These operations carry scoped capabilities, not ordinary host resource
// handles. No driver ABI may infer their meaning from source record names.
struct WorkgroupOperationMetadata {
  WorkgroupAction action;
  NumericType element = NumericType::Float32;
  std::uint64_t count = 0;
};
} // namespace kalandra
