#include "kalandra/WorkgroupGeometry.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace kalandra;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "workgroup geometry assertion %d: %s\n", __LINE__, #condition);           \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)

int main() {
  // Every ordinary output is covered exactly once, irrespective of padding or
  // multidimensional group boundaries. Verify against row-major CPU indexing.
  for (std::int64_t rows = 0; rows <= 9; ++rows)
    for (std::int64_t columns = 0; columns <= 9; ++columns)
      for (std::uint32_t x = 1; x <= 4; ++x)
        for (std::uint32_t y = 1; y <= 4; ++y) {
          const std::int64_t shape[]{rows, columns};
          const std::uint32_t group[]{x, y};
          const auto geometry = makeWorkgroupGeometry(shape, group, 1024, 10000, 10000);
          CHECK(geometry && geometry->elements == std::uint64_t(rows * columns));
          std::vector<unsigned> writes(geometry->elements);
          std::uint64_t active = 0;
          for (std::uint64_t block = 0; block < geometry->groups; ++block)
            for (std::uint32_t lane = 0; lane < geometry->localSize; ++lane) {
              const auto item = workgroupCoordinates(*geometry, block, lane);
              CHECK(item && item->local[0] < x && item->local[1] < y);
              if (item->active) {
                CHECK(item->global[0] < std::uint64_t(rows) &&
                      item->global[1] < std::uint64_t(columns));
                CHECK(item->outputOffset == item->global[0] * columns + item->global[1]);
                ++writes[item->outputOffset];
                ++active;
              }
            }
          CHECK(active == writes.size());
          for (auto count : writes)
            CHECK(count == 1);
          CHECK(!workgroupCoordinates(*geometry, geometry->groups, 0));
          CHECK(!workgroupCoordinates(*geometry, 0, geometry->localSize));
        }
  for (unsigned rank = 0; rank <= 8; ++rank) {
    std::vector<std::int64_t> shape(rank, 2);
    std::vector<std::uint32_t> group(rank, 3);
    const auto geometry = makeWorkgroupGeometry(shape, group, 10000, 10000, 10000);
    CHECK(geometry && geometry->elements == (1u << rank) && geometry->groups == 1);
    std::vector<unsigned> writes(geometry->elements);
    for (std::uint32_t lane = 0; lane < geometry->localSize; ++lane) {
      auto item = workgroupCoordinates(*geometry, 0, lane);
      CHECK(item);
      if (item->active)
        ++writes[item->outputOffset];
    }
    for (auto count : writes)
      CHECK(count == 1);
  }
  using Error = WorkgroupGeometryError;
  const std::int64_t large[]{std::numeric_limits<std::int64_t>::max(), 2};
  const std::uint32_t one[]{1, 1}, two[]{2, 2}, paddedOverflow[]{3, 1}, zero[]{0, 1};
  CHECK(makeWorkgroupGeometry(large, one, 1024, UINT64_MAX, UINT64_MAX));
  CHECK(makeWorkgroupGeometry(large, two, 1024, UINT64_MAX, UINT64_MAX));
  CHECK(makeWorkgroupGeometry(large, paddedOverflow, 1024, UINT64_MAX, UINT64_MAX).error() == Error::Extent);
  CHECK(makeWorkgroupGeometry(large, one, 1024, 100, UINT64_MAX).error() == Error::OutputLimit);
  CHECK(makeWorkgroupGeometry(large, one, 1024, UINT64_MAX, 100).error() == Error::GroupLimit);
  CHECK(makeWorkgroupGeometry(large, two, 3, UINT64_MAX, UINT64_MAX).error() == Error::LocalLimit);
  CHECK(makeWorkgroupGeometry(large, zero, 1024, UINT64_MAX, UINT64_MAX).error() == Error::GroupExtent);
  const std::int64_t overflow[]{std::numeric_limits<std::int64_t>::max(), 3};
  CHECK(makeWorkgroupGeometry(overflow, one, 1024, UINT64_MAX, UINT64_MAX).error() == Error::OutputLimit);
  const std::int64_t empty[]{0, std::numeric_limits<std::int64_t>::max()};
  const auto noLaunch = makeWorkgroupGeometry(empty, two, 1024, 0, 0);
  CHECK(noLaunch && !noLaunch->groups && !noLaunch->elements);
  const std::int64_t negative[]{0, -1};
  CHECK(makeWorkgroupGeometry(negative, one, 1024, 0, 0).error() == Error::Extent);
  CHECK(makeWorkgroupGeometry({}, one, 1024, 1, 1).error() == Error::Rank);
  CHECK(makeWorkgroupGeometry({}, {}, 0, 1, 1).error() == Error::LocalLimit);
  CHECK(makeWorkgroupGeometry({}, {}, 1, 0, 1).error() == Error::OutputLimit);
  CHECK(makeWorkgroupGeometry({}, {}, 1, 1, 0).error() == Error::GroupLimit);
  std::puts("Logical workgroup geometry, padding, exact output coverage and bounds passed");
}
