// Access-order diagnostic; keeps the original complete-cell/guard checks.
#include "../metal/MatmulLayout.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool value) {
  if (!value)
    throw std::runtime_error("weight packing screening failed");
}
int main(int argc, char **argv) {
  @autoreleasepool {
    check(argc == 8 || argc == 9);
    const std::string layout = argv[2];
    const unsigned m = std::stoul(argv[3]), k = std::stoul(argv[4]), n = std::stoul(argv[5]),
                   reuse = std::stoul(argv[6]), rounds = std::stoul(argv[7]);
    check(m && k && n && k % 64 == 0 && k <= 4096 && reuse && reuse <= 16);
    const unsigned iterations = argc == 9 ? std::stoul(argv[8]) : 1;
    check(iterations >= 1 && iterations <= 512);
    const std::string storage = layout.starts_with("block")  ? "block"
                                : layout.starts_with("both") ? "both"
                                                             : "baseline";
    const bool packed = storage != "baseline", left = storage == "both";
    auto device = MTLCreateSystemDefaultDevice();
    check(device);
    NSError *error = nil;
    auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    check(library);
    auto load = [&](std::string name) {
      auto fn = [library newFunctionWithName:@(name.c_str())];
      check(fn);
      auto p = [device newComputePipelineStateWithFunction:fn error:&error];
      check(p && p.maxTotalThreadsPerThreadgroup >= 256);
      return p;
    };
    const auto entry =
        layout == "order_dispatch"
            ? (kalandra::runtime::metal::useGroupedMatmul(m, k, n) ? "order_m16" : "baseline")
            : layout;
    auto matrix = load("matrix_" + entry);
    id<MTLComputePipelineState> packB = packed ? load("pack_" + storage) : nil,
                                packA = left ? load("pack_left") : nil;
    auto queue = [device newCommandQueue];
    check(queue);
    std::uint64_t privateBytes = 0, sharedBytes = 0;
    auto owned = [&](std::size_t count) {
      auto b = [device newBufferWithLength:count options:MTLResourceStorageModePrivate];
      check(b);
      privateBytes += count;
      return b;
    };
    auto shared = [&](std::size_t count) {
      auto b = [device newBufferWithLength:count options:MTLResourceStorageModeShared];
      check(b);
      sharedBytes += count;
      return b;
    };
    const auto ac = std::size_t(m) * k, bc = std::size_t(k) * n, oc = std::size_t(m) * n,
               apc = std::size_t((m + 15) / 16) * 16 * k,
               bpc = std::size_t((n + 15) / 16) * 16 * k +
                     (storage == "blockpad"   ? std::size_t(k / 64) * 32
                      : storage == "panelpad" ? std::size_t((n + 15) / 16) * 32
                                              : 0);
    std::array<id<MTLBuffer>, 2> ah{shared(ac * 4), shared(ac * 4)},
        a{owned(ac * 4), owned(ac * 4)};
    auto bh = shared(bc * 4), b = owned(bc * 4), parameters = shared(12);
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
    id<MTLBuffer> bp = packed ? owned(bpc * 4 + 64) : nil, ap = left ? owned(apc * 4 + 64) : nil;
    std::vector<id<MTLBuffer>> outputs;
    for (unsigned r = 0; r < reuse; ++r)
      outputs.push_back(owned(oc * 4 + 64));
    auto readback = shared(std::max({ac, bc, oc, packed ? bpc : 0, left ? apc : 0}) * 4 + 64);
    auto download = [&](id<MTLBuffer> input) {
      auto cmd = [queue commandBuffer];
      auto e = [cmd blitCommandEncoder];
      [e copyFromBuffer:input
               sourceOffset:0
                   toBuffer:readback
          destinationOffset:0
                       size:input.length];
      [e endEncoding];
      [cmd commit];
      [cmd waitUntilCompleted];
      check(cmd.status == MTLCommandBufferStatusCompleted);
      return static_cast<const std::uint32_t *>(readback.contents);
    };
    std::array<std::array<std::uint32_t, 13>, 17> expected{};
    for (unsigned row = 0; row < 17; ++row)
      for (unsigned col = 0; col < 13; ++col) {
        std::int64_t sum = 0;
        for (unsigned t = 0; t < k; ++t)
          sum += (int((row + t) % 17) - 8) * (int((col + std::uint64_t(t) * n) % 13) - 6);
        expected[row][col] = std::bit_cast<std::uint32_t>(float(sum) / 64);
      }
    for (int round = -2; round < int(rounds); ++round) {
      auto poison = [queue commandBuffer];
      auto fill = [poison blitCommandEncoder];
      for (auto output : outputs)
        [fill fillBuffer:output range:NSMakeRange(0, output.length) value:0xa5];
      for (auto buffer : std::array<id<MTLBuffer>, 2>{bp, ap})
        if (buffer)
          [fill fillBuffer:buffer range:NSMakeRange(0, buffer.length) value:0xa5];
      [fill endEncoding];
      [poison commit];
      [poison waitUntilCompleted];
      check(poison.status == MTLCommandBufferStatusCompleted);
      const auto start = std::chrono::steady_clock::now();
      auto command = [queue commandBuffer];
      auto pack = [&](id<MTLComputePipelineState> pipeline, id<MTLBuffer> input,
                      id<MTLBuffer> output, std::size_t count) {
        auto encoder = [command computeCommandEncoder];
        check(encoder);
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:parameters offset:0 atIndex:2];
#ifdef KALANDRA_PACK_TILED
        const bool packingA = pipeline == packA;
        const auto groups = !packingA && storage == "column"
                                ? MTLSizeMake((n + 31) / 32, k / 32, 1)
                                : MTLSizeMake(((packingA ? m : n) + 15) / 16, k / 64, 1);
        (void)count;
#else
        const auto groups = MTLSizeMake((count + 255) / 256, 1, 1);
#endif
        [encoder dispatchThreadgroups:groups threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [encoder endEncoding];
      };
      for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        if (packed)
          pack(packB, b, bp, bpc);
        for (unsigned r = 0; r < reuse; ++r) {
          if (left)
            pack(packA, a[r % 2], ap, apc);
          auto encoder = [command computeCommandEncoder];
          check(encoder);
          [encoder setComputePipelineState:matrix];
          [encoder setBuffer:left ? ap : a[r % 2] offset:0 atIndex:0];
          [encoder setBuffer:packed ? bp : b offset:0 atIndex:1];
          [encoder setBuffer:outputs[r] offset:0 atIndex:2];
          [encoder setBuffer:parameters offset:0 atIndex:3];
          [encoder dispatchThreadgroups:MTLSizeMake(((std::uint64_t(m) + 15) / 16) *
                                                        ((std::uint64_t(n) + 15) / 16),
                                                    1, 1)
                  threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
          [encoder endEncoding];
        }
      }
      [command commit];
      [command waitUntilCompleted];
      const double ns =
          std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
              .count();
      check(command.status == MTLCommandBufferStatusCompleted &&
            command.GPUEndTime > command.GPUStartTime);
      for (unsigned r = 0; r < reuse; ++r) {
        const auto actual = download(outputs[r]);
        for (unsigned row = 0; row < m; ++row)
          for (unsigned col = 0; col < n; ++col) {
            const auto want = expected[(std::uint64_t(row) * k + (r % 2) * 7) % 17][col % 13];
            if (actual[std::size_t(row) * n + col] != want) {
              std::fprintf(stderr,
                           "%s %u,%u,%u reuse %u output %u cell %u,%u actual=%08x want=%08x\n",
                           layout.c_str(), m, k, n, reuse, r, row, col,
                           actual[std::size_t(row) * n + col], want);
              return 1;
            }
          }
        for (unsigned i = 0; i < 16; ++i)
          check(actual[oc + i] == 0xa5a5a5a5u);
      }
      if (packed) {
        const auto actual = download(bp);
        for (unsigned i = 0; i < k; ++i)
          for (unsigned col = 0; col < ((n + 15) / 16) * 16; ++col) {
            std::size_t index = 0;
            if (storage == "blockpad")
              index = (i / 64) * (std::size_t((n + 15) / 16) * 1024 + 32) + (col / 16) * 1024 +
                      (i % 64) * 16 + col % 16;
            else if (storage == "panelpad")
              index = (col / 16) * (std::size_t(k / 64) * 1024 + 32) + (i / 64) * 1024 +
                      (i % 64) * 16 + col % 16;
            else if (storage == "column")
              index = std::size_t(col) * k + i;
            else if (storage == "block")
              index = ((i / 64) * std::size_t((n + 15) / 16) + col / 16) * 1024 + (i % 64) * 16 +
                      col % 16;
            else if (storage == "tpanel")
              index = ((col / 16) * std::size_t(k / 64) + i / 64) * 1024 + (col % 16) * 64 + i % 64;
            else
              index = ((col / 16) * std::size_t(k / 64) + i / 64) * 1024 + (i % 64) * 16 +
                      (storage == "swizzle" ? ((col % 16) ^ (i % 16)) : col % 16);
            const auto want =
                col < n ? static_cast<const std::uint32_t *>(bh.contents)[std::size_t(i) * n + col]
                        : 0u;
            check(actual[index] == want);
          }
        if (storage == "blockpad")
          for (unsigned tile = 0; tile < k / 64; ++tile)
            for (unsigned i = 0; i < 32; ++i)
              check(actual[tile * (std::size_t((n + 15) / 16) * 1024 + 32) +
                           ((n + 15) / 16) * 1024 + i] == 0u);
        if (storage == "panelpad")
          for (unsigned tile = 0; tile < (n + 15) / 16; ++tile)
            for (unsigned i = 0; i < 32; ++i)
              check(actual[tile * (std::size_t(k / 64) * 1024 + 32) + (k / 64) * 1024 + i] == 0u);
        for (unsigned i = 0; i < 16; ++i)
          check(actual[bpc + i] == 0xa5a5a5a5u);
      }
      if (left) {
        const auto actual = download(ap);
        for (unsigned row = 0; row < ((m + 15) / 16) * 16; ++row)
          for (unsigned i = 0; i < k; ++i) {
            const auto index =
                ((row / 16) * std::size_t(k / 64) + i / 64) * 1024 + (row % 16) * 64 + i % 64;
            const auto want = row < m ? static_cast<const std::uint32_t *>(
                                            ah[(reuse - 1) % 2].contents)[std::size_t(row) * k + i]
                                      : 0u;
            check(actual[index] == want);
          }
        for (unsigned i = 0; i < 16; ++i)
          check(actual[apc + i] == 0xa5a5a5a5u);
      }
      for (unsigned phase = 0; phase < 2; ++phase)
        check(std::memcmp(download(a[phase]), ah[phase].contents, ac * 4) == 0);
      check(std::memcmp(download(b), bh.contents, bc * 4) == 0);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"gpu_ns\":%.0f,\"cells\":%llu,\"workspace_bytes\":%"
                  "llu,\"private_bytes\":%llu,\"shared_bytes\":%llu,\"matrix_max_threads\":%llu,"
                  "\"threadgroup_bytes\":%llu}\n",
                  round, ns, (command.GPUEndTime - command.GPUStartTime) * 1e9,
                  (unsigned long long)oc * reuse,
                  (unsigned long long)((packed ? bpc * 4 + 64 : 0) + (left ? apc * 4 + 64 : 0)),
                  (unsigned long long)privateBytes, (unsigned long long)sharedBytes,
                  (unsigned long long)matrix.maxTotalThreadsPerThreadgroup,
                  (unsigned long long)matrix.staticThreadgroupMemoryLength);
    }
  }
}
