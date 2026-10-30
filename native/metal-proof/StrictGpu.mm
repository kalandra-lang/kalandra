#include "FloatOracle.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#import <dispatch/dispatch.h>
#include <span>
namespace {
constexpr unsigned capacity = 4096, guard = 64, slots = capacity + 2 * guard;
constexpr std::size_t bytes = slots * 4;
constexpr std::uint32_t sentinel = 0x4b123456;
void check(bool good, const char *message) {
  if (!good) {
    std::fprintf(stderr, "Strict Metal: %s\n", message);
    std::exit(1);
  }
}
struct Harness {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> pipeline;
  id<MTLBuffer> a, b, out, uploadA, uploadB, readback, parameters;
  std::uint64_t comparisons = 0, cases = 0, submissions = 0;
  void run(std::span<const float_proof::Pair> pairs, unsigned operation) {
    @autoreleasepool {
      check(pairs.size() <= capacity && operation < 6, "invalid trusted dispatch parameters");
      const unsigned n = pairs.size();
      auto *aBits = static_cast<std::uint32_t *>(uploadA.contents);
      auto *bBits = static_cast<std::uint32_t *>(uploadB.contents);
      auto *outBits = static_cast<std::uint32_t *>(readback.contents);
      std::fill_n(aBits, slots, sentinel);
      std::fill_n(bBits, slots, sentinel);
      std::fill_n(outBits, slots, sentinel);
      for (unsigned i = 0; i < n; ++i) {
        aBits[guard + i] = pairs[i].a;
        bBits[guard + i] = pairs[i].b;
      }
      std::array<std::uint32_t, slots> originalA, originalB;
      std::memcpy(originalA.data(), aBits, bytes);
      std::memcpy(originalB.data(), bBits, bytes);
      std::vector<std::uint32_t> expected(n);
      for (unsigned i = 0; i < n; ++i) {
        expected[i] = float_proof::oracle(pairs[i], operation);
        check(float_proof::equal(expected[i], float_proof::hardware(pairs[i], operation)),
              "CPU hardware/reference disagreement");
      }
      auto *params = static_cast<std::uint32_t *>(parameters.contents);
      params[0] = n;
      params[1] = operation;
      id<MTLCommandBuffer> command = [queue commandBuffer];
      check(command != nil, "command allocation failed");
      id<MTLBlitCommandEncoder> upload = [command blitCommandEncoder];
      check(upload != nil, "upload encoder failed");
      [upload copyFromBuffer:uploadA sourceOffset:0 toBuffer:a destinationOffset:0 size:bytes];
      [upload copyFromBuffer:uploadB sourceOffset:0 toBuffer:b destinationOffset:0 size:bytes];
      [upload copyFromBuffer:readback sourceOffset:0 toBuffer:out destinationOffset:0 size:bytes];
      [upload endEncoding];
      if (n) {
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        check(compute != nil, "compute encoder failed");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:a offset:guard * 4 atIndex:0];
        [compute setBuffer:b offset:guard * 4 atIndex:1];
        [compute setBuffer:out offset:guard * 4 atIndex:2];
        [compute setBuffer:parameters offset:0 atIndex:3];
        [compute dispatchThreadgroups:MTLSizeMake((n + 63) / 64, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [compute endEncoding];
      }
      id<MTLBlitCommandEncoder> download = [command blitCommandEncoder];
      check(download != nil, "download encoder failed");
      [download copyFromBuffer:out sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];
      [download copyFromBuffer:a sourceOffset:0 toBuffer:uploadA destinationOffset:0 size:bytes];
      [download copyFromBuffer:b sourceOffset:0 toBuffer:uploadB destinationOffset:0 size:bytes];
      [download endEncoding];
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [command addCompletedHandler:^(id<MTLCommandBuffer>) {
        dispatch_semaphore_signal(done);
      }];
      [command commit];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
      check(command.status == MTLCommandBufferStatusCompleted, "GPU command failed");
      check(std::memcmp(uploadA.contents, originalA.data(), bytes) == 0 &&
                std::memcmp(uploadB.contents, originalB.data(), bytes) == 0,
            "input changed");
      for (unsigned i = 0; i < slots; ++i) {
        if (i < guard || i >= guard + n)
          check(outBits[i] == sentinel, "guard or inactive output changed");
        else if ((!float_proof::equal(outBits[i], expected[i - guard]) ||
                  (operation == 4 && outBits[i] != expected[i - guard]))) {
          const auto pair = pairs[i - guard];
          std::fprintf(stderr,
                       "Strict GPU mismatch op=%u a=%08x b=%08x expected=%08x actual=%08x\n",
                       operation, pair.a, pair.b, expected[i - guard], outBits[i]);
          std::exit(1);
        }
      }
      comparisons += n;
      ++cases;
      ++submissions;
    }
  }
};
} // namespace
int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: strict-gpu <kernel.metallib> <report.json>\n");
    return 2;
  }
  check(std::fegetround() == FE_TONEAREST, "CPU rounding mode is not nearest/ties-to-even");
  @autoreleasepool {
    Harness h;
    h.device = MTLCreateSystemDefaultDevice();
    check(h.device != nil, "no Metal device available");
    h.queue = [h.device newCommandQueue];
    check(h.queue != nil, "queue creation failed");
    NSError *error = nil;
    id<MTLLibrary> library = [h.device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])]
                                                   error:&error];
    check(library != nil, "library load failed");
    id<MTLFunction> function = [library newFunctionWithName:@"strict_binary32"];
    check(function != nil, "kernel missing");
    h.pipeline = [h.device newComputePipelineStateWithFunction:function error:&error];
    check(h.pipeline != nil && h.pipeline.maxTotalThreadsPerThreadgroup >= 64,
          "pipeline/workgroup unsupported");
    h.a = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.b = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.out = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.uploadA = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.uploadB = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.readback = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.parameters = [h.device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    check(h.a && h.b && h.out && h.uploadA && h.uploadB && h.readback && h.parameters,
          "buffer allocation failed");
    auto corpus = float_proof::corpus();
    const std::span<const float_proof::Pair> pairs(corpus);
    for (unsigned mode = 0; mode < 6; ++mode) {
      for (unsigned n : {0u,   1u,   2u,   31u,  32u,  33u,   63u,   64u,   65u,   127u,
                         128u, 129u, 255u, 256u, 257u, 1023u, 1024u, 1025u, 4095u, 4096u})
        h.run(pairs.first(n), mode);
      for (std::size_t offset = 0; offset < pairs.size(); offset += capacity)
        h.run(pairs.subspan(offset, std::min<std::size_t>(capacity, pairs.size() - offset)), mode);
    }
    auto directed = float_proof::divisionCases();
    const std::span<const float_proof::Pair> divisionPairs(directed);
    for (std::size_t offset = 0; offset < divisionPairs.size(); offset += capacity)
      h.run(divisionPairs.subspan(offset,
                                  std::min<std::size_t>(capacity, divisionPairs.size() - offset)),
            5);
    NSDictionary *report = @{
      @"device" : h.device.name,
      @"pairs" : @(pairs.size()),
      @"directed_division_pairs" : @(divisionPairs.size()),
      @"comparisons" : @(h.comparisons),
      @"cases" : @(h.cases),
      @"submissions" : @(h.submissions),
      @"mismatches" : @0,
      @"input_and_guard_checks_pass" : @YES,
      @"rounding" : @"nearest-ties-to-even",
      @"reference" : @"LLVM APFloat plus CPU hardware",
      @"scope" : @"scalar binary32 add, subtract, multiply, divide, negate, separately-rounded "
                 @"multiply-then-add",
      @"arithmetic" : @"integer MLIR lowering; not hardware float32 arithmetic"
    };
    NSData *json =
        [NSJSONSerialization dataWithJSONObject:report
                                        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                          error:&error];
    check(json != nil && [json writeToFile:@(argv[2]) atomically:YES], "report write failed");
    std::printf("Strict binary32 Metal: %llu comparisons in %llu cases on %s passed\n",
                static_cast<unsigned long long>(h.comparisons),
                static_cast<unsigned long long>(h.cases), h.device.name.UTF8String);
  }
}
