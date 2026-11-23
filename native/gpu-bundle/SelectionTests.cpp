#include "Selection.h"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <thread>
#include <vector>

using namespace kalandra::runtime::bundle;
enum class Error { Invalid, Unavailable, Unsupported, Device };
using Cache = Selection<int, Error>;

bool check(std::string_view forced, Cache::Result cuda, Cache::Result vulkan,
           Cache::Result expected, int cudaCalls, int vulkanCalls) {
  Cache cache;
  std::array<std::atomic<int>, 2> calls{};
  std::atomic<bool> correct = true;
  std::barrier start(16);
  std::vector<std::jthread> tasks;
  for (int i = 0; i < 16; ++i) {
    tasks.emplace_back([&] {
      start.arrive_and_wait();
      for (int n = 0; n < 100; ++n) {
        const auto &got = cache.get(forced, [&](Backend backend) {
          ++calls[backend == Backend::Cuda ? 0 : 1];
          return backend == Backend::Cuda ? cuda : vulkan;
        });
        if (got != expected)
          correct = false;
      }
    });
  }
  tasks.clear();
  return correct && calls[0] == cudaCalls && calls[1] == vulkanCalls;
}
int main() {
  const auto absent = std::unexpected(Error::Unavailable);
  const auto unsupported = std::unexpected(Error::Unsupported);
  const auto fault = std::unexpected(Error::Device);
  if (!check("", 1, 2, 1, 1, 0) || !check("", absent, 2, 2, 1, 1) ||
      !check("", unsupported, 2, 2, 1, 1) || !check("", absent, absent, absent, 1, 1) ||
      !check("vulkan", 1, 2, 2, 0, 1) || !check("cuda", absent, 2, absent, 1, 0) ||
      !check("", fault, 2, fault, 1, 0) ||
      !check("unknown", 1, 2, std::unexpected(Error::Invalid), 0, 0)) {
    std::fputs("GPU bundle selection or probe count mismatch\n", stderr);
    return 1;
  }
}
