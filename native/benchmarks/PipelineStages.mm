// Diagnostic encoder timestamps. Complete managed throughput is measured separately.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool condition) {
  if (!condition)
    throw std::runtime_error("pipeline stage profile failed");
}
int main(int argc, char **argv) {
  @autoreleasepool {
    check(argc == 4);
    const std::string directory = argv[1];
    const unsigned n = std::stoul(argv[2]), rounds = std::stoul(argv[3]);
    check(n >= 512 && n <= 2048 && n % 64 == 0);
    auto device = MTLCreateSystemDefaultDevice();
    check(device && [device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]);
    NSMutableArray *sets = [NSMutableArray array];
    for (id<MTLCounterSet> set in device.counterSets)
      [sets addObject:set.name];
    NSDictionary *metadata = @{
      @"device" : device.name,
      @"counter_sets" : sets,
      @"stage_sampling" :
          @([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]),
      @"dispatch_sampling" :
          @([device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary])
    };
    NSData *encoded = [NSJSONSerialization dataWithJSONObject:metadata options:0 error:nil];
    std::printf("%s\n",
                [[NSString alloc] initWithData:encoded encoding:NSUTF8StringEncoding].UTF8String);
    id<MTLCounterSet> timestamp = nil;
    for (id<MTLCounterSet> set in device.counterSets)
      if ([set.name isEqualToString:MTLCommonCounterSetTimestamp])
        timestamp = set;
    check(timestamp);
    NSError *error = nil;
    auto load = [&](const char *file, const char *entry) {
      auto library =
          [device newLibraryWithURL:[NSURL fileURLWithPath:@((directory + "/" + file).c_str())]
                              error:&error];
      auto function = [library newFunctionWithName:@(entry)];
      auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
      check(pipeline);
      return pipeline;
    };
    auto matrix = load("kernel0-post.metallib", "kalandra_matmul_fused_register16");
    auto reduce = load("kernel0-reduce.metallib", "kalandra_reduce_sum_hardware_block");
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
    const std::size_t count = std::size_t(n) * n;
    auto a = shared(count * 4), b = shared(count * 4), bias = shared(count * 4),
         output = owned(count * 4 + 64), readback = shared(count * 4 + 64), scalar = shared(4);
    auto shape = shared(12), parameters = shared(8);
    std::array dims{n, n, n};
    std::memcpy(shape.contents, dims.data(), 12);
    const std::array<std::uint32_t, 2> words{std::uint32_t(count),
                                             std::bit_cast<std::uint32_t>(0.25f)};
    std::memcpy(parameters.contents, words.data(), 8);
    for (std::size_t i = 0; i < count; ++i) {
      static_cast<float *>(a.contents)[i] = float(int(i % 17) - 8) / 8;
      static_cast<float *>(b.contents)[i] = float(int(i % 13) - 6) / 8;
      static_cast<float *>(bias.contents)[i] = 0.125f;
    }
    std::array<std::array<std::uint32_t, 13>, 17> expected{};
    for (unsigned r = 0; r < 17; ++r)
      for (unsigned c = 0; c < 13; ++c) {
        std::int64_t numerator = 8;
        for (unsigned k = 0; k < n; ++k)
          numerator += (int((r + k) % 17) - 8) * (int((c + std::uint64_t(k) * n) % 13) - 6);
        expected[r][c] = std::bit_cast<std::uint32_t>(float(std::max(INT64_C(0), numerator)) / 256);
      }
    std::vector<id<MTLBuffer>> scratch, counts;
    for (auto length = count; length > 1; length = (length + 127) / 128) {
      scratch.push_back(owned(((length + 127) / 128) * 4));
      counts.push_back(shared(4));
      *static_cast<std::uint32_t *>(counts.back().contents) = std::uint32_t(length);
    }
    auto descriptor = [MTLCounterSampleBufferDescriptor new];
    descriptor.counterSet = timestamp;
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.sampleCount = 4;
    auto counters = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
    check(counters);
    for (bool sampled : {false, true})
      for (int round = -2; round < int(rounds); ++round) {
        auto poison = [queue commandBuffer];
        auto fill = [poison blitCommandEncoder];
        [fill fillBuffer:output range:NSMakeRange(0, output.length) value:0xa5];
        [fill endEncoding];
        [poison commit];
        [poison waitUntilCompleted];
        auto command = [queue commandBuffer];
        auto encoder = [&](unsigned stage) {
          auto pass = [MTLComputePassDescriptor computePassDescriptor];
          pass.dispatchType = MTLDispatchTypeSerial;
          if (sampled) {
            pass.sampleBufferAttachments[0].sampleBuffer = counters;
            pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = stage * 2;
            pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = stage * 2 + 1;
          }
          auto e = [command computeCommandEncoderWithDescriptor:pass];
          check(e);
          return e;
        };
        auto m = encoder(0);
        [m setComputePipelineState:matrix];
        const std::array<id<MTLBuffer>, 6> bindings{a, b, output, shape, bias, parameters};
        for (unsigned i = 0; i < bindings.size(); ++i)
          [m setBuffer:bindings[i] offset:0 atIndex:i];
        [m dispatchThreadgroups:MTLSizeMake(((n + 15) / 16) * ((n + 15) / 16), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [m endEncoding];
        auto r = encoder(1);
        [r setComputePipelineState:reduce];
        auto input = output;
        for (unsigned i = 0; i < scratch.size(); ++i) {
          [r setBuffer:input offset:0 atIndex:0];
          [r setBuffer:scratch[i] offset:0 atIndex:1];
          [r setBuffer:counts[i] offset:0 atIndex:2];
          const auto length = *static_cast<std::uint32_t *>(counts[i].contents);
          [r dispatchThreadgroups:MTLSizeMake((length + 127) / 128, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
          input = scratch[i];
        }
        [r endEncoding];
        auto copy = [command blitCommandEncoder];
        [copy copyFromBuffer:input sourceOffset:0 toBuffer:scalar destinationOffset:0 size:4];
        [copy endEncoding];
        [command commit];
        [command waitUntilCompleted];
        check(command.status == MTLCommandBufferStatusCompleted);
        auto download = [queue commandBuffer];
        auto blit = [download blitCommandEncoder];
        [blit copyFromBuffer:output
                 sourceOffset:0
                     toBuffer:readback
            destinationOffset:0
                         size:output.length];
        [blit endEncoding];
        [download commit];
        [download waitUntilCompleted];
        check(download.status == MTLCommandBufferStatusCompleted);
        const auto actual = static_cast<std::uint32_t *>(readback.contents);
        for (unsigned row = 0; row < n; ++row)
          for (unsigned col = 0; col < n; ++col)
            check(actual[std::size_t(row) * n + col] ==
                  expected[(std::uint64_t(row) * n) % 17][col % 13]);
        for (unsigned i = 0; i < 16; ++i)
          check(actual[count + i] == 0xa5a5a5a5u);
        std::printf("{\"sampled\":%s,\"round\":%d,\"n\":%u,\"result\":%.17g,\"command_ns\":%.0f",
                    sampled ? "true" : "false", round, n,
                    double(*static_cast<float *>(scalar.contents)),
                    (command.GPUEndTime - command.GPUStartTime) * 1e9);
        if (sampled) {
          auto resolved = [counters resolveCounterRange:NSMakeRange(0, 4)];
          check(resolved.length == 4 * sizeof(MTLCounterResultTimestamp));
          auto ticks = static_cast<const MTLCounterResultTimestamp *>(resolved.bytes);
          for (unsigned i = 0; i < 4; ++i)
            check(ticks[i].timestamp != MTLCounterErrorValue);
          check(ticks[1].timestamp >= ticks[0].timestamp &&
                ticks[3].timestamp >= ticks[2].timestamp);
          // Keep raw device ticks. Stage fractions need no assumed clock conversion;
          // whole-command seconds are reported independently by Metal.
          std::printf(",\"matrix_ticks\":%llu,\"reduction_ticks\":%llu,\"span_ticks\":%llu",
                      (unsigned long long)(ticks[1].timestamp - ticks[0].timestamp),
                      (unsigned long long)(ticks[3].timestamp - ticks[2].timestamp),
                      (unsigned long long)(ticks[3].timestamp - ticks[0].timestamp));
        }
        std::printf("}\n");
      }
  }
}
