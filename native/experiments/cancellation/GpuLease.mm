// Physical Metal lifetime test: cleanup must preserve a submitted GPU borrow.
#include "../../runtime/Runtime.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
static void verify(bool value, int line) {
  if (!value) {
    std::fprintf(stderr, "Metal lifetime check failed at %d\n", line);
    std::abort();
  }
}
#define check(value) verify(bool(value), __LINE__)
int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil);
    NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                        "kernel void compute(device const float *a [[buffer(0)]],"
                        "device float *b [[buffer(1)]], uint i [[thread_position_in_grid]])"
                        "{ b[i] = a[i] * 3.0f + 1.0f; }";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    check(library != nil);
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"compute"]
                                              error:&error];
    check(pipeline != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const auto count = page / sizeof(float);
    for (unsigned trial = 0; trial != 100; ++trial) {
      kalandra::runtime::Invocation invocation;
      auto *allocation = invocation.allocate(2 * page);
      check(allocation != nullptr);
      const auto address = (reinterpret_cast<std::uintptr_t>(allocation) + page - 1) & ~(page - 1);
      auto *input = reinterpret_cast<float *>(address);
      for (std::size_t i = 0; i != count; ++i)
        input[i] = float(i + trial);
      auto lease = invocation.lease(allocation);
      check(bool(lease) && lease->referenceCount() == 2);
      id<MTLBuffer> a = [device newBufferWithBytesNoCopy:input
                                                  length:page
                                                 options:MTLResourceStorageModeShared
                                             deallocator:nil];
      id<MTLBuffer> b = [device newBufferWithLength:page options:MTLResourceStorageModeShared];
      check(a != nil && b != nil);
      id<MTLSharedEvent> event = [device newSharedEvent];
      id<MTLCommandBuffer> command = [queue commandBuffer];
      [command encodeWaitForEvent:event value:1];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:a offset:0 atIndex:0];
      [encoder setBuffer:b offset:0 atIndex:1];
      [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
      [command commit];
      // This is the ledger cleanup operation performed after CPU cancellation.
      // The event keeps the submitted GPU operation pending until after cleanup.
      invocation.settle();
      check(invocation.live() == 0 && invocation.allocations() == invocation.releases());
      check(lease->referenceCount() == 1);
      event.signaledValue = 1;
      [command waitUntilCompleted];
      check(command.status == MTLCommandBufferStatusCompleted);
      auto *output = static_cast<const float *>(b.contents);
      for (std::size_t i = 0; i != count; ++i)
        check(output[i] == float(i + trial) * 3.0f + 1.0f);
      // Buffer objects stop borrowing native memory before the final lease dies.
      a = nil;
      command = nil;
      lease.reset();
    }
    std::printf("{\"physical_metal_borrows\":100,\"exact_values\":%zu,\"device\":\"%s\"}\n",
                count * 100, device.name.UTF8String);
  }
}
