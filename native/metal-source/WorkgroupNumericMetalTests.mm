#include "../gpu-source/WorkgroupTestCases.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace kalandra;
int main(int argc, char **argv) {
  if (argc != 2 && argc != 3)
    return 2;
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    auto queue = [device newCommandQueue];
    if (!device || !queue)
      return 1;
    unsigned verified = 0;
    for (const auto &test : qualification::workgroupTestCases(argc == 3)) {
      const auto description = *describeNumeric(test.type);
      const auto rank = test.launch.shape.size();
      const auto &launch = test.launch;
      std::vector<NumericType> tensorTypes;
      std::vector<std::span<const std::uint64_t>> shapes;
      std::vector<id<MTLBuffer>> inputs;
      for (const auto &input : test.inputs) {
        const auto bytes = qualification::workgroupInputBytes(input);
        auto buffer = [device newBufferWithBytes:bytes.data()
                                          length:bytes.size()
                                         options:MTLResourceStorageModeShared];
        if (!buffer)
          return 1;
        inputs.push_back(buffer);
        tensorTypes.push_back(input.type);
        shapes.push_back(input.shape);
      }
      const auto packet = prepareWorkgroupLaunch(launch, {}, {}, shapes, 1024, UINT32_MAX,
                                                 UINT32_MAX, device.maxThreadgroupMemoryLength);
      if (!packet)
        return 1;
      const auto path = std::filesystem::path(argv[1]) / (test.name + ".metallib");
      NSError *error = nil;
      const auto name = workgroupKernelName(description.type, tensorTypes, {}, launch);
      auto library = [device
          newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]
                      error:&error];
      auto function = [library newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]];
      auto pipeline = [device newComputePipelineStateWithFunction:function error:&error];
      const auto bytes = packet->geometry.elements * description.bytes();
      const auto physical = std::max<std::uint64_t>(4, (bytes + 3) / 4 * 4);
      auto output = [device newBufferWithLength:physical options:MTLResourceStorageModeShared];
      auto parameters = [device newBufferWithBytes:packet->parameters.data()
                                            length:packet->parameters.size() * 4
                                           options:MTLResourceStorageModeShared];
      if (!pipeline || !output || !parameters ||
          pipeline.maxTotalThreadsPerThreadgroup < packet->geometry.localSize ||
          pipeline.staticThreadgroupMemoryLength > device.maxThreadgroupMemoryLength)
        return 1;
      std::memset(output.contents, 0, physical);
      auto command = [queue commandBuffer];
      auto encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      for (std::size_t i = 0; i < inputs.size(); ++i)
        [encoder setBuffer:inputs[i] offset:0 atIndex:i];
      [encoder setBuffer:output offset:0 atIndex:inputs.size()];
      [encoder setBuffer:parameters offset:0 atIndex:inputs.size() + 1];
      [encoder dispatchThreadgroups:MTLSizeMake(packet->geometry.groups, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(packet->geometry.localSize, 1, 1)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted ||
          static_cast<const std::uint32_t *>(parameters.contents)[packet->parameters.size() - 1] !=
              UINT32_MAX) {
        std::cerr << description.sourceName << " rank " << rank << " cooperative execution fault\n";
        return 1;
      }
      for (std::uint64_t cell = 0; cell < packet->geometry.elements; ++cell) {
        const auto expected = test.expected[cell];
        std::uint64_t actual = 0;
        std::memcpy(&actual,
                    static_cast<const std::byte *>(output.contents) + cell * description.bytes(),
                    description.bytes());
        if (actual != expected) {
          std::cerr << description.sourceName << " rank " << rank << " cell " << cell
                    << " shared shuffle mismatch\n";
          return 1;
        }
      }
      ++verified;
    }
    std::cout
        << "PASS: " << verified
        << (argc == 3
                ? " bit-exact Metal cooperative caught-error/control fixtures\n"
                : " bit-exact Metal numeric/shared-memory/coordinate kernels, all ten numeric "
                  "types and ranks 0..8 with padding\n");
    return 0;
  }
}
