#pragma once
#include "FormatABI.h"
#include <cstddef>
#include <cstdint>
namespace kalandra::runtime {
using ParseScalarKind = FormatScalarKind;
enum class ParseStatus : std::uint32_t { Success, Invalid, OutOfRange };
// Private v1 transport, independent of the source ParseError representation.
struct ParseScalarResult {
  std::uint64_t bits = 0, offset = 0;
  ParseStatus status = ParseStatus::Success;
};
static_assert(sizeof(ParseScalarResult) == 24 && offsetof(ParseScalarResult, offset) == 8 &&
              offsetof(ParseScalarResult, status) == 16);
} // namespace kalandra::runtime
