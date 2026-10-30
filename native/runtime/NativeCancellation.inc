// Included by Runtime.cpp so standalone runtime users keep one translation unit.
// Only compiler-certified source frames may be abandoned. Runtime helpers and
// scheduler callbacks are never in the eligible executable section.
#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)
#include <csignal>
#include <mutex>
#include <setjmp.h>
#include <sys/ucontext.h>

extern const char kalandraNativeBegin[]
    __asm("section$start$__TEXT$__kalandra") __attribute__((weak_import));
extern const char kalandraNativeEnd[]
    __asm("section$end$__TEXT$__kalandra") __attribute__((weak_import));

namespace kalandra::runtime {
namespace {
struct NativeBoundary {
  void *savedStack;
  Invocation *invocation;
  const std::atomic<bool> *token;
  bool (*body)(void *);
  void *arguments;
#if __has_feature(address_sanitizer)
  sigjmp_buf landing;
#endif
};
static_assert(offsetof(NativeBoundary, savedStack) == 0);
static_assert(offsetof(NativeBoundary, token) == 16);
#if __has_feature(address_sanitizer)
static_assert(offsetof(NativeBoundary, landing) == 40);
#endif
static_assert(std::atomic<bool>::is_always_lock_free);
std::once_flag nativeSignalOnce;
bool nativeSignalReady = false;
alignas(8) std::uint64_t nativeRedirects = 0;

extern "C" bool kalandra_native_enter_v1(bool (*)(void *), void *, NativeBoundary *);
extern "C" bool kalandra_native_enter_cooperative_v1(bool (*)(void *), void *, NativeBoundary *);
extern "C" [[noreturn]] void kalandra_native_cancel_v1();
// Runtime C++ may borrow x28. Reinstall it at EVERY generated callback entry.
// The assembly wrapper owns no C++ resources and restores the caller's register.
asm(R"(
.text
.globl _kalandra_native_enter_v1
.p2align 2
_kalandra_native_enter_v1:
  .cfi_startproc
  stp x29, x30, [sp, #-160]!
  .cfi_def_cfa_offset 160
  .cfi_offset w29, -160
  .cfi_offset w30, -152
  stp x19, x20, [sp, #16]
  stp x21, x22, [sp, #32]
  stp x23, x24, [sp, #48]
  stp x25, x26, [sp, #64]
  stp x27, x28, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  .cfi_offset w19, -144
  .cfi_offset w20, -136
  .cfi_offset w21, -128
  .cfi_offset w22, -120
  .cfi_offset w23, -112
  .cfi_offset w24, -104
  .cfi_offset w25, -96
  .cfi_offset w26, -88
  .cfi_offset w27, -80
  .cfi_offset w28, -72
  .cfi_offset b8, -64
  .cfi_offset b9, -56
  .cfi_offset b10, -48
  .cfi_offset b11, -40
  .cfi_offset b12, -32
  .cfi_offset b13, -24
  .cfi_offset b14, -16
  .cfi_offset b15, -8
  mov x29, sp
  mov x9, sp
  str x9, [x2]
  mov x28, x2
  mov x16, x0
  mov x0, x1
  blr x16
  ldr x28, [sp, #88]
  ldp x29, x30, [sp]
  add sp, sp, #160
  ret
  .cfi_endproc
.globl _kalandra_native_enter_cooperative_v1
.p2align 2
_kalandra_native_enter_cooperative_v1:
  .cfi_startproc
  stp x29, x30, [sp, #-32]!
  .cfi_def_cfa_offset 32
  .cfi_offset w29, -32
  .cfi_offset w30, -24
  str x28, [sp, #16]
  .cfi_offset w28, -16
  mov x29, sp
  mov x28, x2
  mov x16, x0
  mov x0, x1
  blr x16
  ldr x28, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
  .cfi_endproc
.globl _kalandra_native_cancel_v1
.p2align 2
_kalandra_native_cancel_v1:
)");
#if __has_feature(address_sanitizer)
// Preserve ASan's supported stack/fake-stack recovery through its longjmp
// interceptor. Production checkpoints need no returns-twice C++ frame.
asm(R"(
  add x0, x28, #40
  mov w1, #1
  b _siglongjmp
)");
#else
asm(R"(
  ldr x9, [x28]
  mov sp, x9
  ldp x19, x20, [sp, #16]
  ldp x21, x22, [sp, #32]
  ldp x23, x24, [sp, #48]
  ldp x25, x26, [sp, #64]
  ldp x27, x28, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x29, x30, [sp]
  mov w0, #0
  add sp, sp, #160
  ret
)");
#endif

__attribute__((disable_sanitizer_instrumentation))
void nativeInterrupt(int, siginfo_t *, void *opaque) {
  auto &thread = static_cast<ucontext_t *>(opaque)->uc_mcontext->__ss;
  const auto pc = static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(thread));
  // Do not inspect x28 until PC proves the reserved-register contract applies.
  if (pc < reinterpret_cast<std::uintptr_t>(kalandraNativeBegin) ||
      pc >= reinterpret_cast<std::uintptr_t>(kalandraNativeEnd))
    return;
  const auto *boundary = reinterpret_cast<const NativeBoundary *>(thread.__x[28]);
  if (!boundary || !boundary->token ||
      !__atomic_load_n(reinterpret_cast<const bool *>(boundary->token), __ATOMIC_ACQUIRE))
    return;
  // No allocation, unwinding, ledger updates or locks in the signal handler.
  // The normal signal return restores the stack before the regular jump bridge.
  __darwin_arm_thread_state64_set_pc_fptr(thread, &kalandra_native_cancel_v1);
  __atomic_fetch_add(&nativeRedirects, std::uint64_t{1}, __ATOMIC_RELAXED);
}

// The boundary storage belongs to the caller, above the setjmp frame: values
// written by generated code are not indeterminate automatic locals after jump.
#if __has_feature(address_sanitizer)
__attribute__((noinline))
#else
inline
#endif
bool runNativeBoundary(NativeBoundary *boundary) noexcept {
#if __has_feature(address_sanitizer)
  if (sigsetjmp(boundary->landing, 0))
    return false;
#endif
  bool complete = false;
  try {
    complete = kalandra_native_enter_v1(boundary->body, boundary->arguments, boundary) &&
               boundary->invocation->poll() == 0;
  } catch (const SourceAbort &) {
  }
  return complete;
}
} // namespace

bool initializeNativeCancellation() noexcept {
  // TSan defers asynchronous handlers and gives them a copied ucontext. Editing
  // that copy cannot redirect the interrupted thread and may outlive its frame.
  // Keep the normal instrumented cooperative path; never bypass the sanitizer.
  if (!nativeCancellationCanAbandon()) return false;
  try {
    std::call_once(nativeSignalOnce, [] {
      if (+kalandraNativeBegin == +kalandraNativeEnd)
        return;
      struct sigaction previous{};
      if (sigaction(SIGUSR2, nullptr, &previous) || previous.sa_handler != SIG_DFL)
        return;
      struct sigaction action{};
      action.sa_sigaction = nativeInterrupt;
      action.sa_flags = SA_SIGINFO | SA_RESTART;
      sigemptyset(&action.sa_mask);
      nativeSignalReady = sigaction(SIGUSR2, &action, nullptr) == 0;
    });
  } catch (...) {
    return false;
  }
  return nativeSignalReady;
}
bool nativeCancellationCanAbandon() noexcept {
#if __has_feature(thread_sanitizer)
  return false;
#else
  return true;
#endif
}
std::uintptr_t nativeCancellationThread() noexcept {
  sigset_t blocked;
  if (pthread_sigmask(SIG_SETMASK, nullptr, &blocked) || sigismember(&blocked, SIGUSR2))
    return 0;
  return reinterpret_cast<std::uintptr_t>(pthread_self());
}
void interruptNativeCancellation(std::uintptr_t thread) noexcept {
  if (thread)
    pthread_kill(reinterpret_cast<pthread_t>(thread), SIGUSR2);
}
std::uint64_t nativeCancellationRedirects() noexcept {
  return __atomic_load_n(&nativeRedirects, __ATOMIC_RELAXED);
}
} // namespace kalandra::runtime
#else
namespace kalandra::runtime {
bool initializeNativeCancellation() noexcept { return false; }
bool nativeCancellationCanAbandon() noexcept { return false; }
std::uintptr_t nativeCancellationThread() noexcept { return 0; }
void interruptNativeCancellation(std::uintptr_t) noexcept {}
std::uint64_t nativeCancellationRedirects() noexcept { return 0; }
} // namespace kalandra::runtime
#endif

namespace kalandra::runtime {
extern "C" bool kalandra_source_run_v4(Invocation *invocation, bool (*body)(void *),
                                      void *arguments) noexcept {
#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)
  const auto mode = nativeCancellationCanAbandon() ? InvocationABI::armNativeCancellation(*invocation) : 0;
  NativeBoundary boundary;
  boundary.invocation = invocation;
  boundary.token = nullptr;
  boundary.body = body;
  boundary.arguments = arguments;
  if ((mode == 1 || mode == 3) && InvocationABI::nativeCancellationArmed(*invocation)) {
    if (!invocation->enterCall(0, 0)) {
      InvocationABI::restoreNativeCancellation(*invocation, mode);
      return false;
    }
    boundary.token = InvocationABI::cancellationToken(*invocation);
    const bool complete = runNativeBoundary(&boundary);
    invocation->leaveCall();
    InvocationABI::restoreNativeCancellation(*invocation, mode);
    return complete;
  }
  // Even cooperative/unbound callbacks need a valid reserved-register value:
  // a delayed interrupt may arrive after a worker has moved to another task.
  return kalandra_source_run_v3(invocation, [](void *opaque) -> bool {
    auto *boundary = static_cast<NativeBoundary *>(opaque);
    return kalandra_native_enter_cooperative_v1(boundary->body, boundary->arguments, boundary);
  }, &boundary);
#else
  return kalandra_source_run_v3(invocation, body, arguments);
#endif
}
} // namespace kalandra::runtime
