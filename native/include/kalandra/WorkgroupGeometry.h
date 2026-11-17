#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace kalandra {
// Private host/device launch description. Logical ranks are independent of the
// physical backend grid; all physical invocations must preserve padding lanes.
struct WorkgroupGeometry {
  std::uint32_t rank = 0, localSize = 1;
  std::uint64_t elements = 1, groups = 1;
  std::array<std::uint64_t, 8> shape{}, groupsPerAxis{};
  std::array<std::uint32_t, 8> groupShape{};
};
enum class WorkgroupGeometryError {
  Rank,
  Extent,
  GroupExtent,
  LocalLimit,
  OutputLimit,
  GroupLimit
};
std::expected<WorkgroupGeometry, WorkgroupGeometryError>
makeWorkgroupGeometry(std::span<const std::int64_t> shape,
                       std::span<const std::uint32_t> groupShape,
                       std::uint32_t maxLocalSize, std::uint64_t maxElements,
                       std::uint64_t maxGroups);

struct WorkgroupCoordinates {
  std::array<std::uint64_t, 8> global{}, local{};
  bool active = true;
  std::uint64_t outputOffset = 0;
};
// Checked host reference for the exact logical indexing emitted by backends.
// Geometry must come from makeWorkgroupGeometry; out-of-domain invocation IDs
// are invalid, while legitimate padding invocations return active=false.
std::optional<WorkgroupCoordinates> workgroupCoordinates(const WorkgroupGeometry &,
                                                         std::uint64_t group,
                                                         std::uint32_t lane);
} // namespace kalandra
