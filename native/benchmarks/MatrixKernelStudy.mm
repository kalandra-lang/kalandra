// Isolated candidate screening. Final scores use the complete language pipeline.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool ok) {
  if (!ok)
    throw std::runtime_error("matrix kernel screening failed");
}
int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 10)
      return 2;
    const unsigned rows = std::stoul(argv[3]), cols = std::stoul(argv[4]),
                   threads = std::stoul(argv[5]);
    const unsigned m = std::stoul(argv[6]), k = std::stoul(argv[7]), n = std::stoul(argv[8]);
    const unsigned rounds = std::stoul(argv[9]);
    check(m && k && n && k % 64 == 0 && k <= 4096 && rows && cols && threads);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    auto fn = [library newFunctionWithName:@(argv[2])];
    auto pipeline = [device newComputePipelineStateWithFunction:fn error:&error];
    check(pipeline && pipeline.maxTotalThreadsPerThreadgroup >= threads);
    auto queue = [device newCommandQueue];
    auto a = [device newBufferWithLength:std::size_t(m) * k * 4
                                 options:MTLResourceStorageModeShared];
    auto b = [device newBufferWithLength:std::size_t(k) * n * 4
                                 options:MTLResourceStorageModeShared];
    auto out = [device newBufferWithLength:std::size_t(m) * n * 4 + 64
                                   options:MTLResourceStorageModePrivate];
    auto readback = [device newBufferWithLength:out.length options:MTLResourceStorageModeShared];
    const std::array shape{m, k, n};
    auto parameters = [device newBufferWithBytes:shape.data()
                                          length:12
                                         options:MTLResourceStorageModeShared];
    check(queue && a && b && out && readback && parameters);
    auto *av = static_cast<float *>(a.contents), *bv = static_cast<float *>(b.contents);
    for (std::size_t i = 0; i < std::size_t(m) * k; ++i)
      av[i] = float(int(i % 17) - 8) / 8;
    for (std::size_t i = 0; i < std::size_t(k) * n; ++i)
      bv[i] = float(int(i % 13) - 6) / 8;
    std::array<std::array<std::uint32_t, 13>, 17> expected{};
    for (unsigned r = 0; r < 17; ++r)
      for (unsigned c = 0; c < 13; ++c) {
        std::int64_t sum = 0;
        for (unsigned t = 0; t < k; ++t)
          sum += (int((r + t) % 17) - 8) * (int((c + std::uint64_t(t) * n) % 13) - 6);
        expected[r][c] = std::bit_cast<std::uint32_t>(float(sum) / 64);
      }
    for (int round = -2; round < int(rounds); ++round) {
      auto poison = [queue commandBuffer];
      auto fill = [poison blitCommandEncoder];
      [fill fillBuffer:out range:NSMakeRange(0, out.length) value:0xa5];
      [fill endEncoding];
      [poison commit];
      [poison waitUntilCompleted];
      const auto start = std::chrono::steady_clock::now();
      auto command = [queue commandBuffer];
      auto encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:a offset:0 atIndex:0];
      [encoder setBuffer:b offset:0 atIndex:1];
      [encoder setBuffer:out offset:0 atIndex:2];
      [encoder setBuffer:parameters offset:0 atIndex:3];
      [encoder dispatchThreadgroups:MTLSizeMake(((std::uint64_t(m) + rows - 1) / rows) *
                                                    ((std::uint64_t(n) + cols - 1) / cols),
                                                1, 1)
              threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                      .count();
      check(command.status == MTLCommandBufferStatusCompleted &&
            command.GPUEndTime >= command.GPUStartTime);
      auto copy = [queue commandBuffer];
      auto blit = [copy blitCommandEncoder];
      [blit copyFromBuffer:out
               sourceOffset:0
                   toBuffer:readback
          destinationOffset:0
                       size:out.length];
      [blit endEncoding];
      [copy commit];
      [copy waitUntilCompleted];
      check(copy.status == MTLCommandBufferStatusCompleted);
      auto *actual = static_cast<std::uint32_t *>(readback.contents);
      for (unsigned r = 0; r < m; ++r)
        for (unsigned c = 0; c < n; ++c)
          if (actual[std::size_t(r) * n + c] != expected[(std::uint64_t(r) * k) % 17][c % 13]) {
            std::fprintf(stderr, "%s (%u,%u,%u) cell %u,%u actual=%08x expected=%08x\n", argv[2], m,
                         k, n, r, c, actual[std::size_t(r) * n + c],
                         expected[(std::uint64_t(r) * k) % 17][c % 13]);
            return 1;
          }
      for (unsigned i = 0; i < 16; ++i)
        check(actual[std::size_t(m) * n + i] == 0xa5a5a5a5u);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"gpu_ns\":%.0f,\"cells\":%llu,\"register_max_"
                  "threads\":%llu,\"threadgroup_bytes\":%llu}\n",
                  round, ns, (command.GPUEndTime - command.GPUStartTime) * 1e9,
                  (unsigned long long)m * n,
                  (unsigned long long)pipeline.maxTotalThreadsPerThreadgroup,
                  (unsigned long long)pipeline.staticThreadgroupMemoryLength);
    }
  }
}
