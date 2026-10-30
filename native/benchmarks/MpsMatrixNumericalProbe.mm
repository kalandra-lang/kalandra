// Benchmark-only reference: measure numerical behavior, never production code.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void require(bool value) { if(!value)throw std::runtime_error("MPS matrix probe failed"); }
int main(int argc,char **argv) { @autoreleasepool {
  require(argc==7);
  const unsigned m=std::stoul(argv[1]),k=std::stoul(argv[2]),n=std::stoul(argv[3]);
  const std::string policy=argv[6];require(policy=="default" || policy=="full");
  auto device=MTLCreateSystemDefaultDevice();require(device);
  auto queue=[device newCommandQueue];require(queue);
  std::vector<float> a(size_t(m)*k),b(size_t(k)*n),output(size_t(m)*n);
  auto in=fopen(argv[4],"rb");require(in);
  require(fread(a.data(),4,a.size(),in)==a.size() && fread(b.data(),4,b.size(),in)==b.size());
  require(fgetc(in)==EOF);fclose(in);
  auto ab=[device newBufferWithBytes:a.data() length:a.size()*4 options:MTLResourceStorageModeShared];
  auto bb=[device newBufferWithBytes:b.data() length:b.size()*4 options:MTLResourceStorageModeShared];require(ab && bb);
  auto graph=[MPSGraph new];graph.options=MPSGraphOptionsNone;
  auto at=[graph placeholderWithShape:@[@(m),@(k)] dataType:MPSDataTypeFloat32 name:nil];
  auto bt=[graph placeholderWithShape:@[@(k),@(n)] dataType:MPSDataTypeFloat32 name:nil];
  auto result=[graph matrixMultiplicationWithPrimaryTensor:at secondaryTensor:bt name:nil];
  auto ad=[[MPSGraphTensorData alloc] initWithMTLBuffer:ab shape:@[@(m),@(k)] dataType:MPSDataTypeFloat32];
  auto bd=[[MPSGraphTensorData alloc] initWithMTLBuffer:bb shape:@[@(k),@(n)] dataType:MPSDataTypeFloat32];
  auto compilation=[MPSGraphCompilationDescriptor new];
  const auto defaultPrecision=compilation.reducedPrecisionFastMath;
  MPSGraphExecutionDescriptor *execution=nil;
  if(policy=="full") {
    compilation.reducedPrecisionFastMath=MPSGraphReducedPrecisionFastMathNone;
    execution=[MPSGraphExecutionDescriptor new];execution.compilationDescriptor=compilation;
  }
  auto file=fopen(argv[5],"wb");require(file);
  for(unsigned round=0;round<3;++round) {
    auto cb=[MPSCommandBuffer commandBufferFromCommandQueue:queue];
    auto results=[graph encodeToCommandBuffer:cb feeds:@{at:ad,bt:bd} targetTensors:@[result]
                              targetOperations:nil executionDescriptor:execution];
    [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted && results[result]);
    [[results[result] mpsndarray] readBytes:output.data() strideBytes:nil];
    require(fwrite(output.data(),4,output.size(),file)==output.size());
    require(memcmp(ab.contents,a.data(),a.size()*4)==0 && memcmp(bb.contents,b.data(),b.size()*4)==0);
  }
  require(fclose(file)==0);
  printf("{\"rounds\":3,\"cells\":%zu,\"default_reduced_precision\":%lu,\"inputs_unchanged\":true}\n",
         output.size(),(unsigned long)defaultPrecision);
}}
