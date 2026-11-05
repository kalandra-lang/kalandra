#pragma once
#include "kalandra/Semantics.h"
#include <cstddef>

namespace kalandra {
// Imported closures use a canonical tagged carrier and a separate environment
// node. These are device heap layout identities, never native addresses.
struct DeviceClosureImport {
  FunctionId target;
  unsigned carrierLayout, environmentLayout;
  std::size_t carrierWords, environmentWord;
};
} // namespace kalandra
