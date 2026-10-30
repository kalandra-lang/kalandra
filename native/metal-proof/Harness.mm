#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr std::size_t capacity = 4096;
constexpr std::size_t guard = 64; // 256-byte buffer offset, plus a suffix.
constexpr std::size_t slots = capacity + 2 * guard;
constexpr std::size_t bytes = slots * sizeof(float);
constexpr std::uint32_t sentinel = 0x4b123456;

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "Metal proof: %s\n", message);
  std::exit(1);
}
void check(bool condition, const char *message) {
  if (!condition)
    fail(message);
}
float reference(float a, float b) {
  volatile float product = a * b;
  return product + a;
}
bool equivalent(float a, float b) {
  return (std::isnan(a) && std::isnan(b)) ||
         std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}
struct Counts {
  unsigned cases = 0;
  unsigned submissions = 0;
  unsigned edgeMismatches = 0;
  unsigned contractionSensitive = 0;
  unsigned comparisons = 0;
  NSMutableArray *examples = [NSMutableArray array];
};

void run(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLComputePipelineState> pipeline,
         unsigned n, unsigned mode, Counts &counts) {
  @autoreleasepool {
    check(n <= capacity, "invalid test length");
    std::vector<std::uint32_t> a(slots, sentinel), b(slots, sentinel), initial(slots, sentinel);
    std::vector<float> expected(n);
    constexpr std::array<std::uint32_t, 12> edgeA = {
        0x00000001, 0x80000001, 0x007fffff, 0x00800000, 0x80000000, 0x00000000,
        0x7f800000, 0xff800000, 0x7fc12345, 0x7f7fffff, 0x3f800001, 0xbf800001};
    constexpr std::array<std::uint32_t, 12> edgeB = {
        0x3f800000, 0x3f800000, 0x3f000000, 0xbf7fffff, 0x3f800000, 0xbf800000,
        0x00000000, 0x3f800000, 0x3f800000, 0x40000000, 0xbf7ffffe, 0xbf7ffffe};
    std::uint32_t random = 0x125ab329;
    auto next = [&] {
      random = random * 1664525u + 1013904223u;
      return random;
    };
    for (unsigned i = 0; i < n; ++i) {
      if (mode == 1) {
        a[guard + i] = edgeA[i % edgeA.size()];
        b[guard + i] = edgeB[i % edgeB.size()];
      } else if (mode == 2) {
        a[guard + i] = 0x3f800001;
        b[guard + i] = 0xbf7ffffe;
      } else {
        a[guard + i] = (next() & 0x807fffff) | ((100u + next() % 50u) << 23);
        b[guard + i] = (next() & 0x807fffff) | ((100u + next() % 50u) << 23);
      }
      float x = std::bit_cast<float>(a[guard + i]);
      float y = std::bit_cast<float>(b[guard + i]);
      expected[i] = reference(x, y);
      if (!equivalent(expected[i], std::fma(x, y, x)))
        ++counts.contractionSensitive;
    }
    id<MTLBuffer> uploadA = [device newBufferWithBytes:a.data()
                                                length:bytes
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> uploadB = [device newBufferWithBytes:b.data()
                                                length:bytes
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = [device newBufferWithBytes:initial.data()
                                                 length:bytes
                                                options:MTLResourceStorageModeShared];
    id<MTLBuffer> params = [device newBufferWithBytes:&n
                                               length:sizeof(n)
                                              options:MTLResourceStorageModeShared];
    id<MTLBuffer> inputA = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> inputB = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    id<MTLBuffer> output = [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    check(uploadA && uploadB && readback && params && inputA && inputB && output,
          "buffer allocation failed");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    check(command != nil, "command allocation failed");
    id<MTLBlitCommandEncoder> upload = [command blitCommandEncoder];
    check(upload != nil, "upload encoder failed");
    [upload copyFromBuffer:uploadA sourceOffset:0 toBuffer:inputA destinationOffset:0 size:bytes];
    [upload copyFromBuffer:uploadB sourceOffset:0 toBuffer:inputB destinationOffset:0 size:bytes];
    [upload copyFromBuffer:readback sourceOffset:0 toBuffer:output destinationOffset:0 size:bytes];
    [upload endEncoding];
    if (n != 0) {
      id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
      check(compute != nil, "compute encoder failed");
      [compute setComputePipelineState:pipeline];
      [compute setBuffer:inputA offset:guard * sizeof(float) atIndex:0];
      [compute setBuffer:inputB offset:guard * sizeof(float) atIndex:1];
      [compute setBuffer:output offset:guard * sizeof(float) atIndex:2];
      [compute setBuffer:params offset:0 atIndex:3];
      [compute dispatchThreadgroups:MTLSizeMake((n + 63) / 64, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [compute endEncoding];
    }
    id<MTLBlitCommandEncoder> download = [command blitCommandEncoder];
    check(download != nil, "download encoder failed");
    [download copyFromBuffer:output
                sourceOffset:0
                    toBuffer:readback
           destinationOffset:0
                        size:bytes];
    [download copyFromBuffer:inputA sourceOffset:0 toBuffer:uploadA destinationOffset:0 size:bytes];
    [download copyFromBuffer:inputB sourceOffset:0 toBuffer:uploadB destinationOffset:0 size:bytes];
    [download endEncoding];
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(completed);
    }];
    [command commit];
    dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
    if (command.status != MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String);
      fail("GPU command failed");
    }
    ++counts.submissions;
    check(std::memcmp(uploadA.contents, a.data(), bytes) == 0, "first input mutated");
    check(std::memcmp(uploadB.contents, b.data(), bytes) == 0, "second input mutated");
    const auto *result = static_cast<const std::uint32_t *>(readback.contents);
    for (std::size_t i = 0; i < slots; ++i) {
      if (i < guard || i >= guard + n) {
        check(result[i] == sentinel, "output guard or inactive tail modified");
      } else {
        ++counts.comparisons;
        const float value = std::bit_cast<float>(result[i]);
        if (!equivalent(value, expected[i - guard])) {
          if (mode != 1)
            fail("finite/separate-rounding result mismatch");
          ++counts.edgeMismatches;
          if (counts.examples.count < 12) {
            NSDictionary *example = @{
              @"input_a_bits" : @(a[i]),
              @"input_b_bits" : @(b[i]),
              @"expected_bits" : @(std::bit_cast<std::uint32_t>(expected[i - guard])),
              @"actual_bits" : @(result[i])
            };
            if (![counts.examples containsObject:example])
              [counts.examples addObject:example];
          }
        }
      }
    }
    ++counts.cases;
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: metal-proof <kernel.metallib> <report.json>\n");
    return 2;
  }
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "no Metal device available");
    check(device.hasUnifiedMemory, "proof requires Apple unified memory");
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])]
                                                 error:&error];
    if (!library) {
      std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
      fail("could not load generated Metal library");
    }
    id<MTLFunction> function = [library newFunctionWithName:@"multiply_add"];
    check(function != nil, "generated entry point missing");
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                 error:&error];
    check(pipeline != nil, "pipeline creation failed");
    check(pipeline.maxTotalThreadsPerThreadgroup >= 64, "workgroup size unsupported");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "command queue creation failed");
    Counts counts;
    for (unsigned repeat = 0; repeat < 2; ++repeat)
      for (unsigned n : {0u,   1u,   2u,   31u,  32u,  33u,   63u,   64u,   65u,   127u,
                         128u, 129u, 255u, 256u, 257u, 1023u, 1024u, 1025u, 4095u, 4096u})
        for (unsigned mode = 0; mode < 3; ++mode)
          run(device, queue, pipeline, n, mode, counts);
    check(counts.contractionSensitive != 0, "missing contraction-sensitive cases");
    NSDictionary *report = @{
      @"device" : device.name,
      @"unified_memory" : @(device.hasUnifiedMemory),
      @"thread_execution_width" : @(pipeline.threadExecutionWidth),
      @"cases" : @(counts.cases),
      @"submissions" : @(counts.submissions),
      @"comparisons" : @(counts.comparisons),
      @"contraction_sensitive_values" : @(counts.contractionSensitive),
      @"finite_and_separate_rounding_pass" : @YES,
      @"input_and_guard_checks_pass" : @YES,
      @"ieee_edge_mismatches" : @(counts.edgeMismatches),
      @"strict_float32_sample_pass" : @(counts.edgeMismatches == 0),
      @"mismatch_examples" : counts.examples
    };
    NSData *json =
        [NSJSONSerialization dataWithJSONObject:report
                                        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                          error:&error];
    check(json != nil && [json writeToFile:@(argv[2]) atomically:YES], "could not save report");
    std::printf("Metal proof passed: %u cases on %s; IEEE edge mismatches: %u\n", counts.cases,
                device.name.UTF8String, counts.edgeMismatches);
  }
  return 0;
}
