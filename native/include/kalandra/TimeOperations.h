#pragma once
#include <cstdint>
namespace kalandra {
enum class TimeAction : std::uint32_t {
  Now = 1,
  Elapsed,
  Nanoseconds,
  Milliseconds,
  AsNanoseconds,
  AsSeconds,
  Sleep
};
}
