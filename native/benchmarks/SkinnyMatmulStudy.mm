// Preallocated kernel diagnostic. Complete outputs and inputs checked each round.
#include "../metal/MatmulLayout.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool value, std::source_location at = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("skinny matrix study failed at line " + std::to_string(at.line()));
}
int main(int argc, char **argv) {
  @autoreleasepool {
    check(argc == 8);
    const std::string variant = argv[2];
    const unsigned m = std::stoul(argv[3]), k = std::stoul(argv[4]), n = std::stoul(argv[5]),
                   iterations = std::stoul(argv[6]), rounds = std::stoul(argv[7]);
    check(m && n && k && k % 32 == 0 && k <= 16384 && iterations && iterations <= 256);
    auto device = MTLCreateSystemDefaultDevice();
    check(device);
    NSError *error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    check(library);
    unsigned threads = 64, columns = 8;
    std::string entry;
    if (variant == "baseline") {
      const bool reg = kalandra::runtime::metal::useRegisterMatmul(m, k, n);
      const bool wide = !reg && m >= 16 && n >= 8 && k >= 512 && k % 64 == 0;
      entry = reg                       ? "kalandra_matmul_hardware_register16"
              : wide                    ? "kalandra_matmul_hardware_block64"
              : k >= 256 && k % 32 == 0 ? "kalandra_matmul_hardware_block32"
                                        : "kalandra_matmul_hardware_tiled";
      threads = reg ? 256 : wide ? 128 : 64;
      columns = reg ? 16 : 8;
    } else {
      unsigned workers = 0, leaf = 0;
      check(
          std::sscanf(variant.c_str(),
                      variant.starts_with("skinnycol_") ? "skinnycol_%u_%u_%u" : "skinny_%u_%u_%u",
                      &columns, &workers, &leaf) == 3);
      check(columns && workers && leaf && k % leaf == 0);
      threads = columns * workers;
      entry = variant;
    }
    auto function = [library newFunctionWithName:@(entry.c_str())];
    check(function);
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    check(pipeline);
    if (pipeline.maxTotalThreadsPerThreadgroup < threads) {
      std::fprintf(stderr, "%s requires %u threads; pipeline supports %llu\n", entry.c_str(),
                   threads, (unsigned long long)pipeline.maxTotalThreadsPerThreadgroup);
      return 3;
    }
    const auto groups = variant == "baseline"
                            ? ((std::uint64_t(m) + (threads == 256   ? 15
                                                    : threads == 128 ? 15
                                                                     : 7)) /
                               (threads == 64 ? 8 : 16)) *
                                  ((std::uint64_t(n) + columns - 1) / columns)
                            : std::uint64_t(m) * ((std::uint64_t(n) + columns - 1) / columns);
    auto queue = [device newCommandQueue];
    check(queue);
    auto shared = [&](std::size_t bytes) {
      auto b = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
      check(b);
      return b;
    };
    auto owned = [&](std::size_t bytes) {
      auto b = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
      check(b);
      return b;
    };
    const auto ac = std::size_t(m) * k, bc = std::size_t(k) * n, oc = std::size_t(m) * n;
    std::array<id<MTLBuffer>, 2> ah{shared(ac * 4), shared(ac * 4)},
        a{owned(ac * 4), owned(ac * 4)}, output{owned(oc * 4 + 64), owned(oc * 4 + 64)};
    auto bh = shared(bc * 4), b = owned(bc * 4), parameters = shared(12),
         readback = shared(std::max({ac, bc, oc}) * 4 + 64);
    std::array shape{m, k, n};
    std::memcpy(parameters.contents, shape.data(), 12);
    for (unsigned phase = 0; phase < 2; ++phase)
      for (std::size_t i = 0; i < ac; ++i)
        static_cast<float *>(ah[phase].contents)[i] = float(int((i + phase * 7) % 17) - 8) / 8;
    for (std::size_t i = 0; i < bc; ++i)
      static_cast<float *>(bh.contents)[i] = float(int(i % 13) - 6) / 8;
    auto initial = [queue commandBuffer];
    auto upload = [initial blitCommandEncoder];
    for (unsigned phase = 0; phase < 2; ++phase)
      [upload copyFromBuffer:ah[phase]
                sourceOffset:0
                    toBuffer:a[phase]
           destinationOffset:0
                        size:ac * 4];
    [upload copyFromBuffer:bh sourceOffset:0 toBuffer:b destinationOffset:0 size:bc * 4];
    [upload endEncoding];
    [initial commit];
    [initial waitUntilCompleted];
    check(initial.status == MTLCommandBufferStatusCompleted);
    auto download = [&](id<MTLBuffer> input) {
      auto command = [queue commandBuffer];
      auto blit = [command blitCommandEncoder];
      [blit copyFromBuffer:input
               sourceOffset:0
                   toBuffer:readback
          destinationOffset:0
                       size:input.length];
      [blit endEncoding];
      [command commit];
      [command waitUntilCompleted];
      check(command.status == MTLCommandBufferStatusCompleted);
      return static_cast<const std::uint32_t *>(readback.contents);
    };
    std::array<std::array<std::uint32_t, 13>, 17> expected{};
    for (unsigned x = 0; x < 17; ++x)
      for (unsigned y = 0; y < 13; ++y) {
        std::int64_t total = 0;
        for (unsigned i = 0; i < k; ++i)
          total += (int((x + i) % 17) - 8) * (int((y + std::uint64_t(i) * n) % 13) - 6);
        expected[x][y] = std::bit_cast<std::uint32_t>(float(total) / 64);
      }
    for (int round = -2; round < int(rounds); ++round) {
      auto poison = [queue commandBuffer];
      auto fill = [poison blitCommandEncoder];
      for (auto out : output)
        [fill fillBuffer:out range:NSMakeRange(0, out.length) value:0xa5];
      [fill endEncoding];
      [poison commit];
      [poison waitUntilCompleted];
      check(poison.status == MTLCommandBufferStatusCompleted);
      auto start = std::chrono::steady_clock::now();
      auto command = [queue commandBuffer];
      for (unsigned iteration = 0; iteration < iterations; ++iteration)
        for (unsigned phase = 0; phase < 2; ++phase) {
          auto encoder = [command computeCommandEncoder];
          check(encoder);
          [encoder setComputePipelineState:pipeline];
          [encoder setBuffer:a[phase] offset:0 atIndex:0];
          [encoder setBuffer:b offset:0 atIndex:1];
          [encoder setBuffer:output[phase] offset:0 atIndex:2];
          [encoder setBuffer:parameters offset:0 atIndex:3];
          [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                  threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
          [encoder endEncoding];
        }
      [command commit];
      [command waitUntilCompleted];
      auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                    .count();
      check(command.status == MTLCommandBufferStatusCompleted &&
            command.GPUEndTime > command.GPUStartTime);
      for (unsigned phase = 0; phase < 2; ++phase) {
        auto actual = download(output[phase]);
        for (unsigned row = 0; row < m; ++row)
          for (unsigned col = 0; col < n; ++col) {
            const auto want = expected[(std::uint64_t(row) * k + phase * 7) % 17][col % 13];
            if (actual[std::size_t(row) * n + col] != want) {
              std::fprintf(stderr, "%s %u,%u,%u %u,%u: %08x != %08x\n", variant.c_str(), m, k, n,
                           row, col, actual[std::size_t(row) * n + col], want);
              return 1;
            }
          }
        for (unsigned i = 0; i < 16; ++i)
          check(actual[oc + i] == 0xa5a5a5a5u);
        check(std::memcmp(download(a[phase]), ah[phase].contents, ac * 4) == 0);
      }
      check(std::memcmp(download(b), bh.contents, bc * 4) == 0);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"gpu_ns\":%.0f,\"cells\":%llu,\"threads\":%u,"
                  "\"groups\":%llu,\"threadgroup_bytes\":%llu}\n",
                  round, ns, (command.GPUEndTime - command.GPUStartTime) * 1e9,
                  (unsigned long long)oc * 2, threads, (unsigned long long)groups,
                  (unsigned long long)pipeline.staticThreadgroupMemoryLength);
    }
  }
}
