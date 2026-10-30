#include "IntegerOracle.h"
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
#include <string>
namespace {
constexpr unsigned capacity = 4096, guard = 64, slots = capacity + 2 * guard;
constexpr std::size_t bytes = slots * 8;
constexpr std::uint64_t sentinel = UINT64_C(0x4b12345689abcdef);
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
  id<MTLBuffer> a, b, out, uploadA, uploadB, readback, parameters, validity, flags;
  const integer_proof::TypeCase *type = nullptr;
  std::uint64_t comparisons = 0, cases = 0, submissions = 0;
  void run(std::span<const integer_proof::Pair> pairs, unsigned operation) {
    @autoreleasepool {
      check(pairs.size() <= capacity && operation < 6, "invalid trusted dispatch parameters");
      const unsigned n = pairs.size();
      auto *aBits = static_cast<std::uint64_t *>(uploadA.contents);
      auto *bBits = static_cast<std::uint64_t *>(uploadB.contents);
      auto *outBits = static_cast<std::uint64_t *>(readback.contents);
      auto *outFlags = static_cast<std::uint32_t *>(flags.contents);
      std::fill_n(outFlags, slots, 0xdeadbeefu);
      std::fill_n(aBits, slots, sentinel);
      std::fill_n(bBits, slots, sentinel);
      std::fill_n(outBits, slots, sentinel);
      for (unsigned i = 0; i < n; ++i) {
        aBits[guard + i] = pairs[i].a;
        bBits[guard + i] = pairs[i].b;
      }
      std::array<std::uint64_t, slots> originalA, originalB;
      std::memcpy(originalA.data(), aBits, bytes);
      std::memcpy(originalB.data(), bBits, bytes);
      std::vector<integer_proof::Expected> expected(n);
      for (unsigned i = 0; i < n; ++i) {
        expected[i] = integer_proof::oracle(type->type, pairs[i], operation);
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
      [upload copyFromBuffer:flags
                sourceOffset:0
                    toBuffer:validity
           destinationOffset:0
                        size:slots * 4];
      [upload endEncoding];
      if (n) {
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        check(compute != nil, "compute encoder failed");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:a offset:guard * 8 atIndex:0];
        [compute setBuffer:b offset:guard * 8 atIndex:1];
        [compute setBuffer:out offset:guard * 8 atIndex:2];
        [compute setBuffer:validity offset:guard * 4 atIndex:3];
        [compute setBuffer:parameters offset:0 atIndex:4];
        [compute dispatchThreadgroups:MTLSizeMake((n + 63) / 64, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [compute endEncoding];
      }
      id<MTLBlitCommandEncoder> download = [command blitCommandEncoder];
      check(download != nil, "download encoder failed");
      [download copyFromBuffer:out sourceOffset:0 toBuffer:readback destinationOffset:0 size:bytes];
      [download copyFromBuffer:a sourceOffset:0 toBuffer:uploadA destinationOffset:0 size:bytes];
      [download copyFromBuffer:b sourceOffset:0 toBuffer:uploadB destinationOffset:0 size:bytes];
      [download copyFromBuffer:validity
                  sourceOffset:0
                      toBuffer:flags
             destinationOffset:0
                          size:slots * 4];
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
          check(outBits[i] == sentinel && outFlags[i] == 0xdeadbeefu,
                "guard or inactive output changed");
        else if (outFlags[i] != unsigned(expected[i - guard].valid) ||
                 (outFlags[i] && outBits[i] != expected[i - guard].bits)) {
          const auto pair = pairs[i - guard];
          std::fprintf(
              stderr,
              "Strict GPU mismatch op=%u a=%016llx b=%016llx expected=%016llx actual=%016llx\n",
              operation, (unsigned long long)pair.a, (unsigned long long)pair.b,
              (unsigned long long)expected[i - guard].bits, (unsigned long long)outBits[i]);
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
    h.a = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.b = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.out = [h.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    h.uploadA = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.uploadB = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.readback = [h.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    h.parameters = [h.device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    h.validity = [h.device newBufferWithLength:slots * 4 options:MTLResourceStorageModePrivate];
    h.flags = [h.device newBufferWithLength:slots * 4 options:MTLResourceStorageModeShared];
    check(h.a && h.b && h.out && h.uploadA && h.uploadB && h.readback && h.parameters &&
              h.validity && h.flags,
          "buffer allocation failed");
    std::uint64_t totalPairs = 0;
    for (const auto &entry : integer_proof::cases) {
      h.type = &entry;
      auto path = std::string(argv[1]) + "/integer-" + entry.name + ".metallib";
      id<MTLLibrary> library = [h.device newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())]
                                                     error:&error];
      check(library != nil, "integer library load failed");
      auto function = [library newFunctionWithName:@"checked_integer"];
      check(function != nil, "integer entry missing");
      h.pipeline = [h.device newComputePipelineStateWithFunction:function error:&error];
      check(h.pipeline != nil && h.pipeline.maxTotalThreadsPerThreadgroup >= 64,
            "integer pipeline unsupported");
      auto corpus = integer_proof::corpus(entry.type);
      const std::span<const integer_proof::Pair> pairs(corpus);
      totalPairs += pairs.size();
      for (unsigned mode = 0; mode < 6; ++mode) {
        for (unsigned n : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u,
                           1023u, 1024u, 1025u, 4095u, 4096u})
          h.run(pairs.first(n), mode);
        for (std::size_t offset = 0; offset < pairs.size(); offset += capacity)
          h.run(pairs.subspan(offset, std::min<std::size_t>(capacity, pairs.size() - offset)),
                mode);
      }
    }
    NSDictionary *report = @{
      @"device" : h.device.name,
      @"pairs" : @(totalPairs),
      @"comparisons" : @(h.comparisons),
      @"cases" : @(h.cases),
      @"submissions" : @(h.submissions),
      @"mismatches" : @0,
      @"input_and_guard_checks_pass" : @YES,
      @"integer_types" : @8,
      @"reference" : @"LLVM APInt checked signed/unsigned arithmetic",
      @"scope" : @"scalar eight-width integer add, subtract, multiply, divide, remainder, negate; "
                 @"checked faults",
      @"arithmetic" : @"integer MLIR lowering; 64-bit integer limbs; no native double or 128-bit "
                      @"integer arithmetic"
    };
    NSData *json =
        [NSJSONSerialization dataWithJSONObject:report
                                        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                          error:&error];
    check(json != nil && [json writeToFile:@(argv[2]) atomically:YES], "report write failed");
    std::printf("Checked integer Metal: %llu comparisons in %llu cases on %s passed\n",
                static_cast<unsigned long long>(h.comparisons),
                static_cast<unsigned long long>(h.cases), h.device.name.UTF8String);
  }
}
