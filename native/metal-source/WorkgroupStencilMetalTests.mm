#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

// Isolated emitted-kernel execution gate. This deliberately bypasses neither
// arithmetic checks nor padded lanes; host task/lifetime tests are separate.
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    auto library =
        [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                            error:&error];
    auto function = [library newFunctionWithName:@"kalandra_workgroup_v1_float32_tfloat32r1_g256x"];
    auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    auto queue = [device newCommandQueue];
    if (!device || !library || !function || !pipeline || !queue) {
      std::cerr << "Metal cooperative shader admission failed\n";
      return 1;
    }
    for (std::uint32_t count : {1u, 4u, 255u, 256u, 257u, 512u, 1003u}) {
      std::vector<float> values(count);
      for (std::uint32_t i = 0; i < count; ++i)
        values[i] = float(int(i % 37) - 18) * 0.25f;
      auto input = [device newBufferWithBytes:values.data()
                                       length:values.size() * sizeof(float)
                                      options:MTLResourceStorageModeShared];
      auto output = [device newBufferWithLength:values.size() * sizeof(float)
                                        options:MTLResourceStorageModeShared];
      std::array<std::uint32_t, 6> words{(count + 255) / 256, count, 0, count, 0, UINT32_MAX};
      auto parameters = [device newBufferWithBytes:words.data()
                                            length:sizeof(words)
                                           options:MTLResourceStorageModeShared];
      auto command = [queue commandBuffer];
      auto encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:input offset:0 atIndex:0];
      [encoder setBuffer:output offset:0 atIndex:1];
      [encoder setBuffer:parameters offset:0 atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake(words[0], 1, 1)
              threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted ||
          static_cast<const std::uint32_t *>(parameters.contents)[5] != UINT32_MAX) {
        std::cerr << "Cooperative stencil fault at count " << count << '\n';
        return 1;
      }
      auto actual = static_cast<const float *>(output.contents);
      for (std::uint32_t i = 0; i < count; ++i) {
        const auto left = values[i ? i - 1 : 0], right = values[std::min(i + 1, count - 1)];
        const auto expected = ((left + values[i]) + right) / 3.0f;
        if (actual[i] != expected) {
          std::cerr << "Cooperative stencil mismatch: size=" << count << " index=" << i
                    << " expected=" << expected << " actual=" << actual[i] << '\n';
          return 1;
        }
      }
    }
    std::cout << "Metal cooperative stencil: exact independent reference passed, including padded "
                 "groups\n";
  }
}
