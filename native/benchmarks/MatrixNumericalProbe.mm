// Correctness-only probe for frozen benchmark kernels. No production dispatch.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void require(bool value) {
  if (!value) throw std::runtime_error("matrix numerical probe failed");
}
int main(int argc, char **argv) { @autoreleasepool {
  require(argc == 9);
  const std::string variant = argv[2];
  const unsigned m=std::stoul(argv[3]), k=std::stoul(argv[4]), n=std::stoul(argv[5]);
  require(m && n && m%32==0 && n%32==0 && k>=512 && k<=4096 && k%64==0);
  const bool baseline=variant=="baseline", blocked=variant=="blocked";
  require(baseline || blocked || variant=="naive");
  const unsigned tile=baseline?16:blocked?32:8, threads=baseline?256:blocked?128:32;
  auto device=MTLCreateSystemDefaultDevice(); require(device);
  NSError *error=nil;
  auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
  require(library);
  auto function=[library newFunctionWithName:baseline?@"kalandra_matmul_hardware_register16":@"kalandra_simdgroup_matrix"];
  require(function);
  auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];
  require(pipeline && pipeline.maxTotalThreadsPerThreadgroup>=threads);
  require(baseline || pipeline.threadExecutionWidth==32);
  auto queue=[device newCommandQueue]; require(queue);
  const size_t ac=size_t(m)*k, bc=size_t(k)*n, oc=size_t(m)*n;
  auto make=[&](size_t bytes,bool shared) {
    auto value=[device newBufferWithLength:bytes options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];
    require(value); return value;
  };
  auto ah=make(ac*4+64,true), bh=make(bc*4+64,true);
  memset(ah.contents,0xa5,ah.length);memset(bh.contents,0xa5,bh.length);
  auto input=fopen(argv[6],"rb");require(input);
  require(fread(ah.contents,4,ac,input)==ac && fread(bh.contents,4,bc,input)==bc);
  require(fgetc(input)==EOF);fclose(input);
  auto a=make(ah.length,false), b=make(bh.length,false), output=make(oc*4+64,false);
  auto readback=make(std::max({ah.length,bh.length,output.length}),true);
  auto params=make(12,true); std::array shape{m,k,n};memcpy(params.contents,shape.data(),12);
  auto complete=[&](id<MTLCommandBuffer> cb) {
    [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted);
  };
  auto cb=[queue commandBuffer];auto upload=[cb blitCommandEncoder];
  [upload copyFromBuffer:ah sourceOffset:0 toBuffer:a destinationOffset:0 size:ah.length];
  [upload copyFromBuffer:bh sourceOffset:0 toBuffer:b destinationOffset:0 size:bh.length];
  [upload endEncoding];complete(cb);
  auto download=[&](id<MTLBuffer> source) {
    auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:readback destinationOffset:0 size:source.length];
    [blit endEncoding];complete(command);return static_cast<unsigned char*>(readback.contents);
  };
  auto file=fopen(argv[7],"wb");require(file);
  const unsigned rounds=std::stoul(argv[8]);require(rounds==3);
  for(unsigned round=0;round<rounds;++round) {
    auto command=[queue commandBuffer];auto fill=[command blitCommandEncoder];
    [fill fillBuffer:output range:NSMakeRange(0,output.length) value:0xa5];[fill endEncoding];
    auto encoder=[command computeCommandEncoder];require(encoder);
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:a offset:0 atIndex:0];[encoder setBuffer:b offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];[encoder setBuffer:params offset:0 atIndex:3];
    [encoder dispatchThreadgroups:MTLSizeMake(n/tile,m/tile,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];
    [encoder endEncoding];complete(command);
    auto bytes=download(output);
    for(size_t at=oc*4;at<output.length;++at)require(bytes[at]==0xa5);
    require(fwrite(bytes,4,oc,file)==oc);
    require(memcmp(download(a),ah.contents,ah.length)==0);
    require(memcmp(download(b),bh.contents,bh.length)==0);
  }
  require(fclose(file)==0);
  printf("{\"rounds\":3,\"cells\":%zu,\"guards\":true,\"inputs_unchanged\":true}\n",oc);
}}
