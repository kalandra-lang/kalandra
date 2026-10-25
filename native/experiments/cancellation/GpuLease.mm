// Physical Metal lifetime test: cleanup must preserve a submitted GPU borrow.
#include "../../runtime/Runtime.h"
#include "../../runtime/NativeCancellation.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
extern "C" bool kalandra_source_run_v4(kalandra::runtime::Invocation *, bool (*)(void *), void *) noexcept;
struct CpuWork {
  kalandra::runtime::Invocation *invocation;
  std::atomic<bool> started{false}, done{false};
  bool cooperative;
};
__attribute__((section("__TEXT,__kalandra,regular,pure_instructions"), noinline))
static bool cpuWork(void *opaque) {
  auto *work = static_cast<CpuWork *>(opaque);
  work->started.store(true, std::memory_order_release);
  while (true) {
    if (work->cooperative && work->invocation->poll()) return false;
    asm volatile("" ::: "memory");
  }
}
static void verify(bool value, int line) {
  if (!value) {
    std::fprintf(stderr, "Metal lifetime check failed at %d\n", line);
    std::abort();
  }
}
#define check(value) verify(bool(value), __LINE__)
int main() {
  const bool hybrid = kalandra::runtime::initializeNativeCancellation();
  const auto physicalThread = kalandra::runtime::nativeCancellationThread();
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
      // Interrupt CPU work while the submitted Metal operation still borrows
      // this invocation's storage. All Objective-C/C++ owners stay above v4.
      std::atomic<bool> token{false};
      invocation.bindCancellation(&token);
      invocation.allowNativeInterrupts(hybrid && physicalThread);
      CpuWork work{&invocation, {}, {}, !hybrid || !physicalThread};
      std::thread cancel([&] {
        while (!work.started.load(std::memory_order_acquire)) std::this_thread::yield();
        token.store(true, std::memory_order_release);
        while (!work.done.load(std::memory_order_acquire)) {
          if (hybrid && physicalThread)
            kalandra::runtime::interruptNativeCancellation(physicalThread);
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      });
      check(!kalandra_source_run_v4(&invocation, cpuWork, &work));
      work.done.store(true, std::memory_order_release);
      cancel.join();
      check(invocation.callDepth() == 0 && invocation.status() == 0);
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
    check(!hybrid || !physicalThread || kalandra::runtime::nativeCancellationRedirects() >= 100);
    std::printf("{\"physical_metal_borrows\":100,\"exact_values\":%zu,\"cpu_hybrid\":%s,\"redirects\":%llu,\"device\":\"%s\"}\n",
                count * 100, hybrid && physicalThread ? "true" : "false",
                static_cast<unsigned long long>(kalandra::runtime::nativeCancellationRedirects()),
                device.name.UTF8String);
  }
}
