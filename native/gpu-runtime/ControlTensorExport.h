#pragma once
#include "ControlArena.h"
#include <array>

namespace kalandra::runtime::detail {
struct ControlTensorExportRange {
  std::array<std::uint64_t, ControlTensorExportABI::MaxRank> extents{};
  std::uint64_t length, offsetWords;
  unsigned rank;
};
template <class Read>
std::optional<ControlTensorExportRange> controlTensorExportRange(Read read, std::uint64_t arenaWords,
                                                                 unsigned rank) {
  using A = ControlTensorExportABI;
  if (rank > A::MaxRank || arenaWords > ControlArenaABI::MaxWords)
    return {};
  const auto root = read(A::Root), offset = read(A::Data), length = read(A::Length);
  ControlTensorExportRange result{{}, length, offset, rank};
  bool empty = false;
  for (unsigned axis = 0; axis < A::MaxRank; ++axis) {
    const auto extent = read(A::Extents + axis);
    if (extent > ControlArenaABI::MaxWords || (axis >= rank && extent))
      return {};
    result.extents[axis] = extent;
    empty |= axis < rank && extent == 0;
  }
  if (!root || root > arenaWords || 4 + rank > arenaWords - root ||
      offset != root + 4 + rank || length > arenaWords - offset)
    return {};
  std::uint64_t cells = empty ? 0 : 1;
  for (unsigned axis = 0; axis < rank; ++axis) {
    const auto extent = result.extents[axis];
    if (extent && cells > length / extent)
      return {};
    cells *= extent;
  }
  if (cells != length)
    return {};
  return result;
}
} // namespace kalandra::runtime::detail
