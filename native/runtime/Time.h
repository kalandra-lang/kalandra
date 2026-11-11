#pragma once
#include "../include/kalandra/TimeOperations.h"
#include "Runtime.h"
namespace kalandra::runtime {
enum class TimeKind : std::uint32_t { Success, Unavailable, OutOfRange };
struct TimeResult {
  std::uint64_t value = 0;
  TimeKind kind = TimeKind::Success;
};
static_assert(sizeof(TimeResult) == 16 && offsetof(TimeResult, kind) == 8);
class Task;
} // namespace kalandra::runtime
extern "C" {
void kalandra_time_v1(kalandra::runtime::Invocation *, kalandra::runtime::TimeResult *,
                      std::uint32_t action, std::uint64_t value) noexcept;
void kalandra_time_pause_v1(kalandra::runtime::Task *, std::uint64_t deadline) noexcept;
}
