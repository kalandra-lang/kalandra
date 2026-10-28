// Metal-only event/zero-grid witness. Not a language or application benchmark.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>
int main() {
  @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [d newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> l =
        [d newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal; kernel void count(device "
                @"atomic_uint* c [[buffer(0)]],uint i [[thread_position_in_grid]]) {if(i==0) "
                @"atomic_fetch_add_explicit(c,1u,memory_order_relaxed);}"
                        options:nil
                          error:&err];
    id<MTLComputePipelineState> p =
        [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"count"] error:&err];
    if (!p) {
      fprintf(stderr, "%s\n", err.description.UTF8String);
      return 1;
    }
    MTLSharedEventListener *listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0)];
    for (int mode = 0; mode < 3; ++mode) {
      std::vector<double> times;
      for (int repeat = 0; repeat < 34; ++repeat) {
        id<MTLBuffer> c = [d newBufferWithLength:4 options:MTLResourceStorageModeShared];
        *(uint32_t *)c.contents = 0;
        id<MTLBuffer> args = [d newBufferWithLength:16 * 12 options:MTLResourceStorageModeShared];
        auto *a = (uint32_t *)args.contents;
        for (int i = 0; i < 48; ++i)
          a[i] = 1;
        id<MTLSharedEvent> progress = [d newSharedEvent], permission = [d newSharedEvent];
        std::atomic<int> callbacks{0};
        auto *pending = &callbacks;
        auto start = std::chrono::steady_clock::now();
        id<MTLCommandBuffer> command = nil;
        for (int i = 0; i < 16; ++i) {
          if (i == 0 || (mode == 0 && i % 4 == 0))
            command = [q commandBuffer];
          id<MTLComputeCommandEncoder> e = [command computeCommandEncoder];
          [e setComputePipelineState:p];
          [e setBuffer:c offset:0 atIndex:0];
          if (mode)
            [e dispatchThreadgroupsWithIndirectBuffer:args
                                 indirectBufferOffset:i * 12
                                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
          else
            [e dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
          [e endEncoding];
          if (i % 4 == 3 && i != 15 && mode) {
            const int next = i + 1;
            const uint64_t value = next / 4;
            [progress notifyListener:listener
                             atValue:value
                               block:^(id<MTLSharedEvent>, uint64_t) {
                                 if (mode == 2 && value == 1) {
                                   for (int j = next; j < 16; ++j)
                                     a[j * 3] = 0;
                                 }
                                 pending->fetch_add(1);
                                 permission.signaledValue = value;
                               }];
            [command encodeSignalEvent:progress value:value];
            [command encodeWaitForEvent:permission value:value];
          }
          if ((mode == 0 && i % 4 == 3) || i == 15) {
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) {
              fprintf(stderr, "GPU error %s\n", command.error.description.UTF8String);
              return 2;
            }
          }
        }
        auto end = std::chrono::steady_clock::now();
        int expected = mode == 2 ? 4 : 16;
        if (*(uint32_t *)c.contents != expected || (mode && callbacks.load() != 3)) {
          fprintf(stderr, "witness failed %u %d\n", *(uint32_t *)c.contents, callbacks.load());
          return 3;
        }
        if (repeat >= 2)
          times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
      }
      std::sort(times.begin(), times.end());
      printf("mode %d median %.2f us; all witnesses passed\n", mode, times[times.size() / 2]);
    }
  }
}
