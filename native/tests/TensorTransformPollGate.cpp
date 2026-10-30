// Test-only shim for an instrumented copy of generated LLVM. The ordinary
// numerical driver and production runtime keep their original poll calls.
#include "../runtime/Runtime.h"
#include <thread>
using kalandra::runtime::Invocation;
extern "C" std::int64_t kalandra_poll_slow_v1(Invocation *) noexcept;
extern "C" std::int64_t kalandra_transform_poll_gate_v1(Invocation *context) noexcept {
  static thread_local unsigned visits = 0;
  auto status = kalandra_poll_slow_v1(context);
  if (++visits == 32) {
    // The driver observes >=32 polls and then sets its cancellation token.
    // Hold this poll until that observer runs, so completing the whole tensor
    // before its OS timeslice cannot masquerade as a cancellation failure.
    while (!status) {
      std::this_thread::yield();
      status = kalandra_poll_slow_v1(context);
    }
  }
  return status;
}
