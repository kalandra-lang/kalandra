// Benchmark-only optimized library control. No production runtime dependency.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#include "TransformerBlock.h"
int main(int argc,char**argv) { @autoreleasepool {
  ffn::check(argc==8 || argc==9);
  const bool sharedUpload=argc==9 && std::stoul(argv[8])==1;
  const unsigned m=std::stoul(argv[1]),d=std::stoul(argv[2]),count=std::stoul(argv[3]),rounds=std::stoul(argv[4]),mode=std::stoul(argv[5]),dump=std::stoul(argv[6]),pattern=std::stoul(argv[7]);
  ffn::check(m && d && m<=512 && d<=2048 && mode<=2);
  transformer::Inputs data(m,d,pattern);
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> queue=[device newCommandQueue];ffn::check(device && queue);
  MPSGraph*graph=[MPSGraph new];graph.options=MPSGraphOptionsNone;
  NSMutableArray<MPSGraphTensor*>*inputs=[NSMutableArray new];
  for(unsigned i=0;i<7;++i) [inputs addObject:[graph placeholderWithShape:@[@(data.shapes[i][0]),@(data.shapes[i][1])] dataType:MPSDataTypeFloat32 name:nil]];
  auto matmul=[&](MPSGraphTensor*a,MPSGraphTensor*b){return [graph matrixMultiplicationWithPrimaryTensor:a secondaryTensor:b name:nil];};
  auto add=[&](MPSGraphTensor*a,MPSGraphTensor*b){return [graph additionWithPrimaryTensor:a secondaryTensor:b name:nil];};
  auto rms=[&](MPSGraphTensor*x){
    auto square=[graph squareWithTensor:x name:nil];
    auto mean=[graph meanOfTensor:square axes:@[@1] name:nil];
    auto epsilon=[graph constantWithScalar:double(transformer::epsilon) dataType:MPSDataTypeFloat32];
    auto denominator=[graph squareRootWithTensor:add(mean,epsilon) name:nil];
    return [graph divisionWithPrimaryTensor:x secondaryTensor:denominator name:nil];
  };
  auto norm=rms(inputs[0]);auto q=matmul(norm,inputs[1]);auto k=matmul(norm,inputs[2]);auto v=matmul(norm,inputs[3]);
  auto kt=[graph transposeTensor:k dimension:0 withDimension:1 name:nil];
  auto scale=[graph constantWithScalar:double(data.scale) dataType:MPSDataTypeFloat32];
  auto scores=[graph multiplicationWithPrimaryTensor:matmul(q,kt) secondaryTensor:scale name:nil];
  auto probability=[graph softMaxWithTensor:scores axis:1 name:nil];
  auto residual=add(inputs[0],matmul(matmul(probability,v),inputs[4]));
  auto hidden=[graph reLUWithTensor:matmul(rms(residual),inputs[5]) name:nil];
  auto output=add(residual,matmul(hidden,inputs[6]));
  NSMutableArray<id<MTLBuffer>>*host=[NSMutableArray new];
  NSMutableArray<MPSGraphTensorData*>*resident=[NSMutableArray new];
  auto upload=[&](unsigned i){
    if(sharedUpload) {
      // Matched owned-snapshot control: include allocation and CPU copy in
      // timed uploads, with no alias to the original host backing.
      id<MTLBuffer> buffer=[device newBufferWithBytes:host[i].contents length:host[i].length options:MTLResourceStorageModeShared];ffn::check(bool(buffer));
      return [[MPSGraphTensorData alloc] initWithMTLBuffer:buffer shape:@[@(data.shapes[i][0]),@(data.shapes[i][1])] dataType:MPSDataTypeFloat32];
    }
    id<MTLBuffer> buffer=[device newBufferWithLength:data.values[i].size()*4 options:MTLResourceStorageModePrivate];ffn::check(bool(buffer));
    auto cb=[queue commandBuffer];auto encoder=[cb blitCommandEncoder];
    [encoder copyFromBuffer:host[i] sourceOffset:0 toBuffer:buffer destinationOffset:0 size:buffer.length];[encoder endEncoding];[cb commit];[cb waitUntilCompleted];ffn::check(cb.status==MTLCommandBufferStatusCompleted);
    return [[MPSGraphTensorData alloc] initWithMTLBuffer:buffer shape:@[@(data.shapes[i][0]),@(data.shapes[i][1])] dataType:MPSDataTypeFloat32];
  };
  for(unsigned i=0;i<7;++i) {
    auto b=[device newBufferWithBytes:data.values[i].data() length:data.values[i].size()*4 options:MTLResourceStorageModeShared];ffn::check(bool(b));[host addObject:b];
    // Placeholder entries keep indexing stable for per-invocation uploads.
    [resident addObject:(!mode || (mode==1 && i))?upload(i):[[MPSGraphTensorData alloc] initWithMTLBuffer:b shape:@[@(data.shapes[i][0]),@(data.shapes[i][1])] dataType:MPSDataTypeFloat32]];
  }
  std::uint64_t gpuNs=0;
  auto call=[&](bool save) { @autoreleasepool {
    NSMutableDictionary<MPSGraphTensor*,MPSGraphTensorData*>*feeds=[NSMutableDictionary new];
    for(unsigned i=0;i<7;++i) feeds[inputs[i]]=(mode==2 || (mode==1 && !i))?upload(i):resident[i];
    auto cb=[MPSCommandBuffer commandBufferFromCommandQueue:queue];
    auto results=[graph encodeToCommandBuffer:cb feeds:feeds targetTensors:@[output] targetOperations:nil executionDescriptor:nil];
    [cb commit];[cb waitUntilCompleted];ffn::check(cb.status==MTLCommandBufferStatusCompleted && results[output]);
    if(cb.GPUEndTime>=cb.GPUStartTime) gpuNs+=std::uint64_t((cb.GPUEndTime-cb.GPUStartTime)*1e9);
    if(save) {std::vector<float> cells(m*d);[[results[output] mpsndarray] readBytes:cells.data() strideBytes:nil];ffn::dump(cells,ffn::sum(cells));}
    return double(m)*d*4;
  }};
  // Graph optimization and pipeline compilation are preparation, excluded
  // from warm invocation comparisons for both implementations.
  if(dump) {call(true);return 0;}
  for(int round=-2;round<int(rounds);++round) {
    gpuNs=0;double checksum=0;auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<count;++i) checksum+=call(false);
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"gpu_ns\":%llu}\n",round,ffn::elapsed(start),checksum,(unsigned long long)gpuNs);
  }
}}
