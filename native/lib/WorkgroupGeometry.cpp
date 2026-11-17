#include "kalandra/WorkgroupGeometry.h"
#include <limits>

namespace kalandra {
std::expected<WorkgroupGeometry, WorkgroupGeometryError>
makeWorkgroupGeometry(std::span<const std::int64_t> shape,
                       std::span<const std::uint32_t> groupShape,
                       std::uint32_t maxLocalSize, std::uint64_t maxElements,
                       std::uint64_t maxGroups) {
  using Error = WorkgroupGeometryError;
  if (shape.size() > 8 || shape.size() != groupShape.size())
    return std::unexpected(Error::Rank);
  WorkgroupGeometry geometry;
  geometry.rank = static_cast<std::uint32_t>(shape.size());
  bool empty = false;
  for (std::size_t axis = 0; axis < shape.size(); ++axis) {
    if (shape[axis] < 0)
      return std::unexpected(Error::Extent);
    if (!groupShape[axis])
      return std::unexpected(Error::GroupExtent);
    if (groupShape[axis] > maxLocalSize / geometry.localSize)
      return std::unexpected(Error::LocalLimit);
    geometry.localSize *= groupShape[axis];
    geometry.shape[axis] = static_cast<std::uint64_t>(shape[axis]);
    geometry.groupShape[axis] = groupShape[axis];
    empty |= shape[axis] == 0;
  }
  if (geometry.localSize > maxLocalSize)
    return std::unexpected(Error::LocalLimit);
  geometry.elements = geometry.groups = empty ? 0 : 1;
  for (std::size_t axis = 0; axis < shape.size(); ++axis) {
    const auto extent = geometry.shape[axis];
    const auto size = geometry.groupShape[axis];
    const auto count = extent / size + (extent % size != 0);
    geometry.groupsPerAxis[axis] = count;
    if (empty)
      continue;
    // Every padding invocation also exposes an i64 global index. Reject a
    // rounded domain whose final coordinate would not fit that source type.
    const auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (count && count - 1 > (maximum - (size - 1)) / size)
      return std::unexpected(Error::Extent);
    if (extent > maxElements / geometry.elements)
      return std::unexpected(Error::OutputLimit);
    geometry.elements *= extent;
    if (count > maxGroups / geometry.groups)
      return std::unexpected(Error::GroupLimit);
    geometry.groups *= count;
  }
  // Rank zero has one element and one invocation, not an empty launch.
  if (geometry.elements > maxElements)
    return std::unexpected(Error::OutputLimit);
  if (geometry.groups > maxGroups)
    return std::unexpected(Error::GroupLimit);
  return geometry;
}

std::optional<WorkgroupCoordinates> workgroupCoordinates(const WorkgroupGeometry &geometry,
                                                         std::uint64_t group,
                                                         std::uint32_t lane) {
  if (group >= geometry.groups || lane >= geometry.localSize)
    return {};
  WorkgroupCoordinates result;
  for (std::size_t axis = geometry.rank; axis-- > 0;) {
    result.local[axis] = lane % geometry.groupShape[axis];
    lane /= geometry.groupShape[axis];
    const auto block = group % geometry.groupsPerAxis[axis];
    group /= geometry.groupsPerAxis[axis];
    result.global[axis] = block * geometry.groupShape[axis] + result.local[axis];
    result.active &= result.global[axis] < geometry.shape[axis];
  }
  if (result.active)
    for (std::size_t axis = 0; axis < geometry.rank; ++axis)
      result.outputOffset = result.outputOffset * geometry.shape[axis] + result.global[axis];
  return result;
}
} // namespace kalandra
