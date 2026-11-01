#include "Vulkan.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
using namespace kalandra::runtime::vulkan;
int main() {
  std::atomic<bool> failed = false;
  std::array<std::thread, 8> callers;
  for (auto &caller : callers)
    caller = std::thread([&] {
      for (unsigned i = 0; i < 4; ++i) {
        auto devices = Runtime::enumerate(false);
        if (devices || devices.error() != Error::Unavailable)
          failed = true;
        auto runtime = Runtime::create({.validation = false});
        if (runtime || runtime.error() != Error::Unavailable)
          failed = true;
      }
    });
  for (auto &caller : callers) caller.join();
  if (failed) return 1;
  std::puts("PASS: concurrent absent/incomplete loader reports Unavailable before Vulkan entry");
}
