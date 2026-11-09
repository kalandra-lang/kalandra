#pragma once
#include "NumericType.h"
#include <span>
#include <string>
namespace kalandra {
// Private typed map entry spelling includes complete heterogeneous capture
// identities. Packed int8/int16/int32 buffers otherwise share a physical uint
// reflection type; reflection alone cannot distinguish those interfaces.
inline std::string numericKernelName(NumericType result, std::span<const NumericType> tensors,
                                     std::span<const NumericType> scalars) {
  std::string name = "kalandra_numeric_v1_";
  name += describeNumeric(result)->name;
  bool homogeneous = true;
  for (auto type : tensors)
    homogeneous &= type == result;
  for (auto type : scalars)
    homogeneous &= type == result;
  if (homogeneous)
    return name;
  // Large control closures can carry many identical scalar slots. Preserve
  // every type and its position with a lossless run spelling rather than
  // exceeding a backend's entry-name bound. Short names keep their old ABI.
  const auto signature = [](std::span<const NumericType> types) {
    std::string plain;
    for (auto type : types)
      plain += char('a' + static_cast<unsigned>(type));
    if (plain.empty())
      return std::string("z");
    if (plain.size() < 64)
      return plain;
    std::string runs = "r";
    for (std::size_t i = 0; i < plain.size();) {
      auto end = i + 1;
      while (end < plain.size() && plain[end] == plain[i])
        ++end;
      runs += std::to_string(end - i);
      runs += plain[i];
      i = end;
    }
    return runs.size() < plain.size() ? runs : plain;
  };
  name += "_t_" + signature(tensors);
  name += "_s_" + signature(scalars);
  return name;
}
} // namespace kalandra
