#include "../runtime/Runtime.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#endif
using kalandra::runtime::Invocation;
#ifdef FLOAT_TAIL
extern "C" double _mlir_ciface_kalandra_test_entry(double, std::int64_t, Invocation *);
#else
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t,
                                                        std::int64_t, std::int64_t, Invocation *);
#endif
static void check(bool value) {
  if (!value)
    throw std::runtime_error("tail-call conformance failed");
}
#ifndef FLOAT_TAIL
static std::int64_t expected(int kind, std::int64_t a, std::int64_t b, std::int64_t n) {
  if (kind == 0) {
    while (b) {
      const auto r = a == std::numeric_limits<std::int64_t>::min() && b == -1 ? 0 : a % b;
      a = b;
      b = r;
    }
    return a;
  }
  if (kind == 1) {
    if (n % 2)
      std::swap(a, b);
    return a * 1000 + b;
  }
  if (kind == 2) {
    while (n--) {
      if (a % 2 == 0) {
        a += b;
        b = -b;
      } else {
        a -= b;
      }
    }
    return a;
  }
  if (kind == 3)
    return a + n * (n + 1) / 2;
  if (kind == 4)
    return a / b;
  return a + n;
}
#endif
#if defined(__APPLE__) && !defined(FLOAT_TAIL)
__attribute__((noinline)) static void lowStack(unsigned n, Invocation &ctx) {
  const auto bytes = std::size_t(8192 + n % 16);
  auto *memory = static_cast<volatile char *>(__builtin_alloca(bytes));
  for (std::size_t i = 0; i < bytes; i += 4096)
    memory[i] = char(n);
  if (n)
    lowStack(n - 1, ctx);
  else
    check(_mlir_ciface_kalandra_test_entry(1, 37, 7, 10, &ctx) == 0 && ctx.status() == 6);
  check(memory[0] == char(n));
}
struct StackCase { Invocation *ctx; bool low; bool passed = false; };
static void *stackWorker(void *pointer) {
  auto &test = *static_cast<StackCase *>(pointer);
  try {
    if (test.low)
      lowStack(18, *test.ctx);
    else
      check(_mlir_ciface_kalandra_test_entry(1, 37, 7, 70, test.ctx) == 37007 &&
            test.ctx->status() == 0 && test.ctx->callDepth() == 1);
    test.passed = true;
  } catch (...) {}
  return nullptr;
}
#endif
int main() {
  unsigned cases = 0;
  for (int mode = 0; mode < 7; ++mode)
    for (int manual : {0, 1, 120, 124, 125, 126, 127})
      for (int n : {0, 1, 2, 7, 16, 119, 120, 124, 125, 126, 127, 128, 1000}) {
#ifdef FLOAT_TAIL
        for (auto bits : {0ULL, 0x8000000000000000ULL, 0x3ff4000000000000ULL,
                          0xbff4000000000000ULL, 1ULL, 0x7ff0000000000000ULL,
                          0xfff0000000000000ULL, 0x7ff8000000000042ULL}) {
          const auto input = std::bit_cast<double>(std::uint64_t(bits));
#else
        for (int kind : {0, 1, 2, 3, 4, 5, 7, 8})
          for (auto b : {0, 1, -1, 7, -9}) {
            const std::int64_t a = 37;
#endif
            Invocation ctx;
            std::atomic<bool> token = mode == 4;
            std::atomic<std::uint64_t> polls = 0;
            for (int i = 0; i < manual; ++i)
              check(ctx.enterCall(0, 0));
            if (mode == 1 || mode == 3 || mode == 4)
              ctx.bindCancellation(&token);
            if (mode == 2 || mode == 3)
              ctx.watchPolls(&polls);
            if (mode == 5)
              ctx.fault(5, 19, 23);
            if (mode == 6)
              ctx.settle();
#ifdef FLOAT_TAIL
            const auto value = _mlir_ciface_kalandra_test_entry(input, n, &ctx);
            const auto result = std::bit_cast<std::uint64_t>(value);
            if (!ctx.status() && !token)
              check(result == (bits ^ (n % 2 ? 0x8000000000000000ULL : 0)));
#else
            const auto value = _mlir_ciface_kalandra_test_entry(kind, a, b, n, &ctx);
            const auto result = std::uint64_t(value);
            if (!ctx.status() && !token)
              check(value == expected(kind, a, b, n));
#endif
            // Independent assertions remain effective without --baseline.
            if (mode == 5)
              check(ctx.status() == 5 && ctx.line() == 19 && ctx.column() == 23 && result == 0);
            else if (mode == 6)
              check(ctx.status() == 3 && result == 0);
            else if (mode == 4)
              check(ctx.status() == 0 && result == 0);
            else {
#ifdef FLOAT_TAIL
              check(ctx.status() == 0);
#else
              check(ctx.status() == (kind == 4 && b == 0 ? 4 : 0));
#endif
            }
            if (mode == 2 || mode == 3)
              check(polls > 0);
            check(ctx.callDepth() == std::uint64_t(manual) && ctx.live() == 0);
            std::printf("%llu %lld %lld %lld %llu %llu\n",
                        static_cast<unsigned long long>(result), static_cast<long long>(ctx.status()),
                        static_cast<long long>(ctx.line()), static_cast<long long>(ctx.column()),
                        static_cast<unsigned long long>(polls.load()),
                        static_cast<unsigned long long>(ctx.callDepth()));
            for (int i = 0; i < manual; ++i)
              ctx.leaveCall();
            ctx.settle();
            check(ctx.live() == 0 && ctx.callDepth() == 0);
            ++cases;
          }
      }
#ifndef FLOAT_TAIL
  #if defined(__APPLE__)
  Invocation migrating;
  check(migrating.enterCall(0, 0));
  for (int i = 0; i < 12; ++i) {
    pthread_attr_t attributes;
    check(!pthread_attr_init(&attributes));
    check(!pthread_attr_setstacksize(&attributes, i % 2 ? 256 * 1024 : 512 * 1024));
    StackCase test{&migrating, false};
    pthread_t thread;
    check(!pthread_create(&thread, &attributes, stackWorker, &test));
    check(!pthread_join(thread, nullptr) && !pthread_attr_destroy(&attributes));
    check(test.passed && migrating.callDepth() == 1);
    check(_mlir_ciface_kalandra_test_entry(1, 37, 7, 70, &migrating) == 37007 &&
          migrating.callDepth() == 1);
  }
  migrating.leaveCall();
  {
    Invocation ctx;
    pthread_attr_t attributes;
    check(!pthread_attr_init(&attributes) && !pthread_attr_setstacksize(&attributes, 256 * 1024));
    StackCase test{&ctx, true};
    pthread_t thread;
    check(!pthread_create(&thread, &attributes, stackWorker, &test));
    check(!pthread_join(thread, nullptr) && !pthread_attr_destroy(&attributes));
    check(test.passed && ctx.callDepth() == 0 && ctx.status() == 6);
  }
  #endif
  for (auto a : {std::numeric_limits<std::int64_t>::min(),
                 std::numeric_limits<std::int64_t>::max(), std::int64_t(-37)})
    for (std::int64_t b : {0, 1, -1, 7, -9}) {
      Invocation ctx;
      std::atomic<std::uint64_t> polls = 0;
      ctx.watchPolls(&polls);
      const auto value = _mlir_ciface_kalandra_test_entry(0, a, b, 0, &ctx);
      check(ctx.status() == 0 && value == expected(0, a, b, 0) && ctx.callDepth() == 0);
      std::printf("gcd-edge %lld %llu\n", static_cast<long long>(value),
                  static_cast<unsigned long long>(polls.load()));
      ++cases;
    }
  // Token cancellation while re-entering tail loops through a large tree.
  for (bool observed : {false, true}) {
    Invocation ctx;
    std::atomic<bool> token = false;
    std::atomic<std::uint64_t> polls = 0;
    ctx.bindCancellation(&token);
    if (observed)
      ctx.watchPolls(&polls);
    std::thread stop([&] {
      if (observed)
        while (polls.load() < 1000)
          std::this_thread::yield();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      token.store(true, std::memory_order_release);
    });
    check(_mlir_ciface_kalandra_test_entry(6, 37, 7, 22, &ctx) == 0);
    stop.join();
    check(ctx.status() == 0 && ctx.poll() == -1 && ctx.callDepth() == 0 && ctx.live() == 0);
  }
#endif
  std::fprintf(stderr, "%u independent-reference and logical-state cases passed\n", cases);
}
