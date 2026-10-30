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
  name += "_t_";
  if(tensors.empty()) name+='z';
  for (auto type : tensors)
    name += char('a' + static_cast<unsigned>(type));
  name += "_s_";
  if(scalars.empty()) name+='z';
  for (auto type : scalars)
    name += char('a' + static_cast<unsigned>(type));
  return name;
}
} // namespace kalandra
