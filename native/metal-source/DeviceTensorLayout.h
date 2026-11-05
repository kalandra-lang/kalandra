#pragma once
#include "kalandra/Semantics.h"
#include <expected>
#include <span>

namespace kalandra {
struct DeviceTensorLayout {
  std::vector<std::uint64_t> extents, strides;
  std::uint64_t elements = 1;
};

inline std::expected<DeviceTensorLayout, std::string>
planDeviceTensorLayout(std::span<const Extent> shape, std::uint64_t maxWords) {
  DeviceTensorLayout layout;
  if (!maxWords || shape.size() > 8)
    return std::unexpected("checkpointed tensor rank or budget is unsupported");
  layout.extents.resize(shape.size());
  layout.strides.resize(shape.size());
  for (std::size_t i = shape.size(); i-- != 0;) {
    if (!shape[i].constant)
      return std::unexpected("checkpointed tensor extent must be statically known");
    const auto extent = *shape[i].constant;
    if (extent > maxWords || (extent && layout.elements > maxWords / extent))
      return std::unexpected("checkpointed tensor shape exceeds the arena word budget");
    layout.extents[i] = extent;
    layout.strides[i] = layout.elements;
    layout.elements *= extent;
  }
  return layout;
}
} // namespace kalandra
