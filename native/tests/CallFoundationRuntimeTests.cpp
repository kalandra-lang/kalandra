#include "../runtime/Runtime.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif
#if defined(_WIN32) && defined(__aarch64__)
static volatile int foundationStage = 0;
#define FOUNDATION_STAGE(value) (foundationStage = (value))
static LONG CALLBACK foundationException(EXCEPTION_POINTERS *exception) {
  if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    const auto *context = exception->ContextRecord;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(stderr, "ARM64 foundation AV stage=%d PC=%llx RVA=%llx SP=%llx FP=%llx LR=%llx x19=%llx x20=%llx access=%llu address=%llx\n",
                 foundationStage, static_cast<unsigned long long>(context->Pc),
                 static_cast<unsigned long long>(context->Pc - base),
                 static_cast<unsigned long long>(context->Sp), static_cast<unsigned long long>(context->Fp),
                 static_cast<unsigned long long>(context->Lr), static_cast<unsigned long long>(context->X[19]),
                 static_cast<unsigned long long>(context->X[20]),
                 static_cast<unsigned long long>(exception->ExceptionRecord->ExceptionInformation[0]),
                 static_cast<unsigned long long>(exception->ExceptionRecord->ExceptionInformation[1]));
    std::fflush(stderr);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
#define FOUNDATION_STAGE(value) ((void)0)
#endif
using kalandra::runtime::Invocation;
extern "C" bool kalandra_source_run_v3(Invocation *, bool (*)(void *), void *) noexcept;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
static void check(bool ok, int line) {
  if (!ok)
    throw std::runtime_error("call foundation failure at " + std::to_string(line));
}
#define CHECK(value) check(bool(value), __LINE__)
static auto call(Invocation &ctx, int kind, std::int64_t n) {
  return _mlir_ciface_kalandra_test_entry(kind, n, &ctx);
}
static void settled(Invocation &ctx) {
  CHECK(ctx.callDepth() == 0);
  ctx.settle();
  CHECK(ctx.live() == 0 && ctx.allocations() == ctx.releases());
}
static void smallStack(bool &passed) {
  try {
    for (int kind : {0, 1, 2, 3, 7, 8}) {
      FOUNDATION_STAGE(700 + kind);
      Invocation ctx;
      const auto result = call(ctx, kind, 1000000);
      CHECK(result == (kind == 1 ? 1 : kind == 3 ? 3000000 : kind == 7 ? 1000042 : kind == 8 ? 1000036 : 1000000));
      CHECK(ctx.status() == 0);
      settled(ctx);
    }
    FOUNDATION_STAGE(710);
    Invocation ctx;
    CHECK(call(ctx, 4, 1000000) == 0 && ctx.status() == 6);
    settled(ctx);
    passed = true;
  } catch (...) {
  }
}
#if defined(_WIN32)
static DWORD WINAPI stackWorker(void *argument) {
  smallStack(*static_cast<bool *>(argument));
  return 0;
}
#else
static void *stackWorker(void *argument) {
  smallStack(*static_cast<bool *>(argument));
  return nullptr;
}
#endif
int main() {
#if defined(_WIN32) && defined(__aarch64__)
  AddVectoredExceptionHandler(1, foundationException);
#endif
  FOUNDATION_STAGE(1);
  {
    Invocation ctx;
    std::atomic<bool> token{false};
    ctx.bindCancellation(&token);
    struct State { Invocation *context; std::atomic<bool> *token; } state{&ctx, &token};
    auto stopped = [](void *pointer) -> bool {
      auto &state = *static_cast<State *>(pointer);
      state.token->store(true, std::memory_order_release);
      const bool ready = state.context->poll() == 0;
      state.token->store(false, std::memory_order_release);
      return ready;
    };
    CHECK(!kalandra_source_run_v3(&ctx, stopped, &state));
    CHECK(ctx.status() == 0 && ctx.poll() == 0);
    settled(ctx);
  }
  for (int mode = 0; mode != 3; ++mode)
    for (int kind : {0, 1, 2, 3, 7, 8}) {
      Invocation ctx;
      std::atomic<bool> token{false};
      std::atomic<std::uint64_t> polls{0};
      if (mode == 1) ctx.bindCancellation(&token);
      if (mode == 2) ctx.watchPolls(&polls);
      const auto expected = kind == 1 ? 1 : kind == 3 ? 3000000 : kind == 7 ? 1000042 : kind == 8 ? 1000036 : 1000000;
      FOUNDATION_STAGE(100 + mode * 10 + kind);
      const auto result = call(ctx, kind, 1000000);
      if (result != expected || ctx.status())
        std::fprintf(stderr, "mode=%d kind=%d result=%lld expected=%d status=%lld at %lld:%lld\n",
                      mode, kind, static_cast<long long>(result), expected,
                      static_cast<long long>(ctx.status()), static_cast<long long>(ctx.line()),
                      static_cast<long long>(ctx.column()));
      CHECK(result == expected && ctx.status() == 0);
      settled(ctx);
    }
  for (int mode = 0; mode != 3; ++mode) {
    Invocation ctx;
    std::atomic<bool> token{false};
    std::atomic<std::uint64_t> polls{0};
    if (mode == 1) ctx.bindCancellation(&token);
    if (mode == 2) ctx.watchPolls(&polls);
    double expected = 1;
    for (int n = 0; n != 1024; ++n) {
      volatile double product = expected * 1.000000001;
      expected = product + 0.25;
    }
    FOUNDATION_STAGE(200 + mode * 10);
    CHECK(call(ctx, 4, 1024) == std::int64_t(expected) && ctx.status() == 0);
    FOUNDATION_STAGE(201 + mode * 10);
    CHECK(call(ctx, 5, 1000000) == 0 && ctx.status() == 4 && ctx.line() > 0);
    settled(ctx);
    FOUNDATION_STAGE(202 + mode * 10);
    Invocation deep;
    CHECK(call(deep, 6, 512) == 0 && deep.status() == 4);
    settled(deep);
  }
  {
    Invocation ctx;
    FOUNDATION_STAGE(300);
    for (unsigned n = 0; n != 10000; ++n) CHECK(ctx.enterCall(1, 2));
    CHECK(call(ctx, 0, 1000000) == 1000000 && ctx.status() == 0 && ctx.callDepth() == 10000);
    for (unsigned n = 0; n != 10000; ++n) ctx.leaveCall();
    settled(ctx);
  }
  for (bool observed : {false, true}) {
    Invocation ctx;
    std::atomic<bool> token{false};
    std::atomic<std::uint64_t> polls{0};
    ctx.bindCancellation(&token);
    if (observed) ctx.watchPolls(&polls);
    std::thread stop([&] {
      if (observed) {
        while (polls.load() < 1000) std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      token.store(true, std::memory_order_release);
    });
    FOUNDATION_STAGE(400 + observed);
    const auto result = call(ctx, 0, std::numeric_limits<std::int64_t>::max());
    stop.join();
    CHECK(result == 0 && ctx.status() == 0 && ctx.poll() == -1);
    settled(ctx);
  }
  {
    FOUNDATION_STAGE(500);
    Invocation failed;
    failed.fault(5, 17, 19);
    CHECK(call(failed, 3, 1000000) == 0 && failed.status() == 5 && failed.line() == 17 &&
          failed.column() == 19 && failed.allocations() == 0);
    settled(failed);
    Invocation closed;
    closed.settle();
    CHECK(call(closed, 0, 1) == 0 && closed.status() == 3 && closed.callDepth() == 0);
  }
  FOUNDATION_STAGE(600);
  bool passed = false;
#if defined(_WIN32)
  auto thread = CreateThread(nullptr, 512 * 1024, stackWorker, &passed,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  CHECK(thread && WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
  CHECK(CloseHandle(thread));
#else
  pthread_attr_t attributes;
  CHECK(!pthread_attr_init(&attributes));
  CHECK(!pthread_attr_setstacksize(&attributes, 512 * 1024));
  pthread_t thread;
  CHECK(!pthread_create(&thread, &attributes, stackWorker, &passed));
  CHECK(!pthread_join(thread, nullptr));
  CHECK(!pthread_attr_destroy(&attributes));
#endif
  CHECK(passed);
  std::puts("million-step self/mutual/indirect/captured tail calls; deep non-tail arithmetic, "
            "stack exhaustion, cancellation, boundary nesting and cleanup passed");
}
