#pragma once
#include "String.h"
namespace kalandra::runtime {
// Private transport result. Faults/cancellation stay in Invocation, not IoError.
enum class ConsoleKind : std::uint32_t { Success, End, System, InvalidEncoding, LineTooLong };
struct ConsoleResult {
  ConsoleKind kind = ConsoleKind::Success;
  std::int32_t code = 0;
  StringDescriptor text;
};
} // namespace kalandra::runtime
extern "C" {
void kalandra_console_write_v1(kalandra::runtime::Invocation *, kalandra::runtime::ConsoleResult *,
                               const kalandra::runtime::StringDescriptor *, std::uint32_t stream,
                               bool newline) noexcept;
void kalandra_console_read_line_v1(kalandra::runtime::Invocation *,
                                   kalandra::runtime::ConsoleResult *,
                                   std::uint64_t maxBytes) noexcept;
}

// A pending operation owns only invocation control state; the OS service owns
// its fixed mailbox. A task may resume on another worker or cancel and retire.
extern "C" {
bool kalandra_console_write_step_v1(kalandra::runtime::Invocation *,
                                    kalandra::runtime::ConsoleResult *,
                                    const kalandra::runtime::StringDescriptor *,
                                    std::uint32_t stream, bool newline, void **state) noexcept;
bool kalandra_console_read_line_step_v1(kalandra::runtime::Invocation *,
                                        kalandra::runtime::ConsoleResult *, std::uint64_t maxBytes,
                                        void **state) noexcept;
}

namespace kalandra::runtime {
class Task;
}
extern "C" void kalandra_console_pause_v1(kalandra::runtime::Task *, void *state) noexcept;
