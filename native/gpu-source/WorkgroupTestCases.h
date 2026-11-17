#pragma once
#include "kalandra/WorkgroupLaunch.h"
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace kalandra::qualification {
struct WorkgroupTestInput {
  NumericType type;
  std::vector<std::uint64_t> shape, bits;
};
struct WorkgroupTestCase {
  std::string name;
  NumericType type;
  WorkgroupLaunch launch;
  std::vector<std::uint64_t> expected;
  std::vector<WorkgroupTestInput> inputs{};
};
inline std::vector<std::byte> workgroupInputBytes(const WorkgroupTestInput &input) {
  const auto width = describeNumeric(input.type)->bytes();
  std::vector<std::byte> bytes(input.bits.size() * width);
  for (std::size_t i = 0; i < input.bits.size(); ++i)
    std::memcpy(bytes.data() + i * width, &input.bits[i], width);
  return bytes;
}
inline WorkgroupTestCase tiledWorkgroupCase() {
  WorkgroupTestCase test{
      "tiled",
      NumericType::Float32,
      {{4, 4},
       {2, 2},
       {{{WorkgroupShapeAction::Constant, 7}}, {{WorkgroupShapeAction::Constant, 5}}},
       128},
      {}};
  WorkgroupTestInput a{NumericType::Float32, {7, 8}, {}}, b{NumericType::Float32, {8, 5}, {}};
  for (unsigned i = 0; i < 56; ++i)
    a.bits.push_back(std::bit_cast<std::uint32_t>(float(int(i % 7) - 3)));
  for (unsigned i = 0; i < 40; ++i)
    b.bits.push_back(std::bit_cast<std::uint32_t>(float(int(i % 11) - 5)));
  // Independent straight dot products, without the device's tiled algorithm.
  for (unsigned row = 0; row < 7; ++row)
    for (unsigned column = 0; column < 5; ++column) {
      int total = 0;
      for (unsigned k = 0; k < 8; ++k)
        total += (int((row * 8 + k) % 7) - 3) * (int((k * 5 + column) % 11) - 5);
      test.expected.push_back(std::bit_cast<std::uint32_t>(float(total)));
    }
  test.inputs = {std::move(a), std::move(b)};
  return test;
}
inline std::vector<WorkgroupTestCase> workgroupTestCases(bool controls) {
  std::vector<WorkgroupTestCase> cases;
  if (controls) {
    for (const auto &[name, values] : std::vector<std::pair<std::string, std::vector<float>>>{
             {"caught-lane-error-before-barrier", {3, 2, 1, 7}},
             {"caught-error-payload", {7, 1, 2, 3}},
             {"caught-conditional-error-value", {3, 7, 1, 7}},
             {"failed-argument-skips-shared-helper", {11, 10, 9, 8}},
             {"varying-failed-argument-skips-store", {106, 198, 198, 100}},
             {"nested-return-skips-divide-fault", {7, 5, 5, 5}}}) {
      WorkgroupTestCase test{
          name,
          NumericType::Float32,
          {{4},
           {},
           {{{WorkgroupShapeAction::Constant, 4}}},
           (name == "caught-error-payload" || name == "nested-return-skips-divide-fault") ? 0u
                                                                                          : 16u},
          {}};
      for (auto value : values)
        test.expected.push_back(std::bit_cast<std::uint32_t>(value));
      cases.push_back(std::move(test));
    }
    cases.push_back(tiledWorkgroupCase());
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
  for (auto description : numericTypes) {
    const auto width = description.bits;
    const auto mask = width == 64 ? UINT64_MAX : (std::uint64_t(1) << width) - 1;
    const auto sign = std::uint64_t(1) << (width - 1);
    std::vector<std::uint64_t> bits;
    if (description.type == NumericType::Float32)
      bits = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc12345, 1};
    else if (description.type == NumericType::Float64)
      bits = {0,
              0x8000000000000000ULL,
              0x3ff0000000000000ULL,
              0xbff0000000000000ULL,
              0x7ff0000000000000ULL,
              0xfff0000000000000ULL,
              0x7ff8123456789abcULL,
              1};
    else
      bits = {0, mask, sign, sign - 1, 1, 2, mask - 1, sign / 2};
    WorkgroupTestCase test{
        std::string(description.sourceName) + "-bit-patterns",
        description.type,
        {{8}, {1}, {{{WorkgroupShapeAction::Constant, 9}}}, 8 * (width == 64 ? 8u : 4u)},
        {}};
    test.inputs.push_back({description.type, {8}, bits});
    for (unsigned i = 0; i < 9; ++i)
      test.expected.push_back(bits[7 - (i % 8)]);
    cases.push_back(std::move(test));
  }
  return cases;
}
} // namespace kalandra::qualification
