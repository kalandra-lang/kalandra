// Test-only direct submission adapter: source algorithms/collector execute in
// the unmodified emitted shader. Host graph generation is the independent oracle.
#define KALANDRA_TRACE_PHYSICAL
#include "DeviceTraceExecutionTests.cpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
struct PhysicalTrace {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLComputePipelineState> pipeline;
  PhysicalTrace() {
    device = MTLCreateSystemDefaultDevice();
    require(device != nil, "physical Metal device missing");
    const char *file = std::getenv("KALANDRA_TRACE_METALLIB");
    require(file != nullptr, "set KALANDRA_TRACE_METALLIB");
    NSError *error = nil;
    auto library =
        [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:file]]
                            error:&error];
    require(library != nil, "trace library could not load");
    auto function = [library newFunctionWithName:@"kalandra_numeric_v1_uint64_d_t"];
    pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    require(pipeline != nil, "trace pipeline could not prepare");
    queue = [device newCommandQueue];
    std::cout << "Physical device: " << [[device name] UTF8String] << '\n';
  }
};
extern "C" void _mlir_ciface_step(Memref<std::uint64_t> *heap, Memref<std::uint32_t> *frame) {
  @autoreleasepool {
    static PhysicalTrace runtime;
    auto arena = [runtime.device newBufferWithBytes:heap->aligned
                                             length:heap->size * sizeof(std::uint64_t)
                                            options:MTLResourceStorageModeShared];
    auto params = [runtime.device newBufferWithBytes:frame->aligned
                                              length:frame->size * sizeof(std::uint32_t)
                                             options:MTLResourceStorageModeShared];
    auto output = [runtime.device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    require(arena && params && output, "trace test buffer allocation failed");
    auto command = [runtime.queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:runtime.pipeline];
    [encoder setBuffer:arena offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBuffer:params offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "trace command failed");
    std::memcpy(heap->aligned, arena.contents, heap->size * sizeof(std::uint64_t));
    std::memcpy(frame->aligned, params.contents, frame->size * sizeof(std::uint32_t));
  }
}
