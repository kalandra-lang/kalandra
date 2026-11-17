#pragma once
#include "kalandra/WorkgroupLaunch.h"
#include <bit>
#include <string>
#include <vector>

namespace kalandra::qualification {
struct WorkgroupTestCase {
  std::string name;
  NumericType type;
  WorkgroupLaunch launch;
  std::vector<std::uint64_t> expected;
};
inline std::vector<WorkgroupTestCase> workgroupTestCases(bool controls) {
  std::vector<WorkgroupTestCase> cases;
  if (controls) {
    for (const auto &[name, values] : std::vector<std::pair<std::string, std::vector<float>>>{
             {"caught-lane-error-before-barrier", {3, 2, 1, 7}},
             {"caught-error-payload", {7, 1, 2, 3}}}) {
      WorkgroupTestCase test{name,
                             NumericType::Float32,
                             {{4},
                              {},
                              {{{WorkgroupShapeAction::Constant, 4}}},
                              name == "caught-error-payload" ? 0u : 16u},
                             {}};
      for (auto value : values)
        test.expected.push_back(std::bit_cast<std::uint32_t>(value));
      cases.push_back(std::move(test));
    }
    return cases;
  }
  for (auto description : numericTypes)
    for (unsigned rank = 0; rank <= 8; ++rank) {
      WorkgroupTestCase test{std::string(description.sourceName) + "-rank-" + std::to_string(rank),
                             description.type,
                             {},
                             {}};
      for (unsigned axis = 0; axis < rank; ++axis) {
        test.launch.groupShape.push_back(2);
        test.launch.shape.push_back({{WorkgroupShapeAction::Constant, axis ? 2u : 3u}});
      }
      test.launch.scratchBytes = (1u << rank) * (description.bits == 64 ? 8 : 4);
      const auto count = rank ? 3u * (1u << (rank - 1)) : 1u;
      for (unsigned cell = 0; cell < count; ++cell) {
        const auto value = ((1u << rank) - 1 - (cell % (1u << rank))) % 3 + 1;
        test.expected.push_back(
            description.type == NumericType::Float32   ? std::bit_cast<std::uint32_t>(float(value))
            : description.type == NumericType::Float64 ? std::bit_cast<std::uint64_t>(double(value))
                                                       : value);
      }
      cases.push_back(std::move(test));
    }
  return cases;
}
} // namespace kalandra::qualification
