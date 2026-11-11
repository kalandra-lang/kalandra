#include "Time.h"
#include "Tasks.h"
#include <bit>
#include <chrono>
#include <limits>

namespace {
using Clock = std::chrono::steady_clock;
using Wide = unsigned __int128;
using kalandra::runtime::TimeKind;
using kalandra::runtime::TimeResult;
static_assert(Clock::is_steady && std::is_integral_v<Clock::rep> && sizeof(Clock::rep) <= 8);

bool ready(kalandra::runtime::Invocation &context) noexcept {
  return !context.poll() && context.usable();
}
std::optional<std::uint64_t> instant() noexcept {
  const auto ticks = Clock::now().time_since_epoch().count();
  if constexpr (std::is_signed_v<Clock::rep>)
    if (ticks < 0)
      return {};
  const auto nanos = Wide(ticks) * Clock::period::num * 1'000'000'000 / Clock::period::den;
  if (nanos > UINT64_MAX)
    return {};
  return static_cast<std::uint64_t>(nanos);
}
TimeResult prepareSleep(std::uint64_t nanoseconds) noexcept {
  const auto now = Clock::now().time_since_epoch().count();
  if constexpr (std::is_signed_v<Clock::rep>)
    if (now < 0)
      return {0, TimeKind::Unavailable};
  // Round up to the next clock tick so a coarse native clock cannot shorten a sleep.
  const auto divisor = Wide(Clock::period::num) * 1'000'000'000;
  const auto ticks = (Wide(nanoseconds) * Clock::period::den + divisor - 1) / divisor;
  const auto deadline = Wide(now) + ticks;
  if (deadline > static_cast<Wide>(std::numeric_limits<Clock::rep>::max()))
    return {0, TimeKind::OutOfRange};
  return {static_cast<std::uint64_t>(deadline), TimeKind::Success};
}
} // namespace
extern "C" void kalandra_time_v1(kalandra::runtime::Invocation *context,
                                 kalandra::runtime::TimeResult *output, std::uint32_t action,
                                 std::uint64_t input) noexcept {
  using kalandra::TimeAction;
  *output = {};
  if (!ready(*context))
    return;
  TimeResult result;
  switch (static_cast<TimeAction>(action)) {
  case TimeAction::Now:
  case TimeAction::Elapsed: {
    const auto current = instant();
    if (!current)
      result.kind = TimeKind::Unavailable;
    else if (static_cast<TimeAction>(action) == TimeAction::Now)
      result.value = *current;
    else if (input > *current)
      result.kind = TimeKind::OutOfRange;
    else
      result.value = *current - input;
    break;
  }
  case TimeAction::Nanoseconds:
  case TimeAction::AsNanoseconds:
    result.value = input;
    break;
  case TimeAction::Milliseconds:
    if (input > UINT64_MAX / 1'000'000)
      result.kind = TimeKind::OutOfRange;
    else
      result.value = input * 1'000'000;
    break;
  case TimeAction::AsSeconds:
    result.value = std::bit_cast<std::uint64_t>(static_cast<double>(input) / 1'000'000'000.0);
    break;
  case TimeAction::Sleep:
    result = prepareSleep(input);
    break;
  default:
    context->fault(3);
    break;
  }
  if (ready(*context))
    *output = result;
}
extern "C" void kalandra_time_pause_v1(kalandra::runtime::Task *task,
                                       std::uint64_t deadline) noexcept {
  if (deadline > static_cast<std::uint64_t>(std::numeric_limits<Clock::rep>::max())) {
    task->fault(3);
    return;
  }
  task->suspendUntil(Clock::time_point(Clock::duration(static_cast<Clock::rep>(deadline))));
}
