// Isolated kernel screen. No language dispatch policy or arithmetic changes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool value) { if (!value) throw std::runtime_error("output reuse screen failed"); }
int main(int argc, char **argv) { @autoreleasepool {
  check(argc == 9);
  const bool candidate = std::string(argv[2]) == "current";
  check(candidate || std::string(argv[2]) == "baseline");
  unsigned m=std::stoul(argv[3]), k=std::stoul(argv[4]), n=std::stoul(argv[5]),
           count=std::stoul(argv[6]), rounds=std::stoul(argv[7]), pattern=std::stoul(argv[8]);
  check(m && n && k>=512 && k<=4096 && k%64==0 && count && count<=256 && pattern<=2);
  auto device=MTLCreateSystemDefaultDevice(); check(device); NSError *error=nil;
  auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error]; check(library);
  auto make=[&](const char *name) { auto f=[library newFunctionWithName:@(name)]; check(f);
    auto p=[device newComputePipelineStateWithFunction:f error:&error]; check(p);
    check(p.maxTotalThreadsPerThreadgroup>=256); return p; };
  auto reference=make("kalandra_matmul_hardware_register16");
  auto compute=make(candidate ? "kalandra_matmul_hardware_pair32" : "kalandra_matmul_hardware_register16");
  auto queue=[device newCommandQueue]; check(queue);
  auto buffer=[&](size_t bytes,bool shared=false) {auto b=[device newBufferWithLength:bytes options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];check(b);return b;};
  size_t ac=size_t(m)*k, bc=size_t(k)*n, oc=size_t(m)*n;
  auto ah=buffer(ac*4,true), bh=buffer(bc*4,true), a=buffer(ac*4), b=buffer(bc*4),
       out=buffer(oc*4+64), control=buffer(oc*4+64), readback=buffer(std::max({ac,bc,oc})*4+64,true), params=buffer(12,true);
  std::array shape{m,k,n}; memcpy(params.contents,shape.data(),12);
  float *av=(float*)ah.contents,*bv=(float*)bh.contents;
  constexpr std::array<uint32_t,12> ieee{0,0x80000000,0x3f800000,0xbf800000,1,0x80000001,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0xffc12345};
  for(size_t i=0;i<ac;++i) av[i]=pattern==1?std::bit_cast<float>(ieee[(i+i/k)%ieee.size()]):pattern==2?-0.0f:float((int(i%17)-8)*0.134567+(i%29==0?10000.0003:0.00043));
  for(size_t i=0;i<bc;++i) bv[i]=pattern==1?std::bit_cast<float>(ieee[(i*7+i/n)%ieee.size()]):pattern==2?1.0f:float((int(i%13)-6)*0.173219+(i%31==0?-0.000023:0.00019));
  auto initial=[queue commandBuffer];auto upload=[initial blitCommandEncoder];
  [upload copyFromBuffer:ah sourceOffset:0 toBuffer:a destinationOffset:0 size:ac*4];
  [upload copyFromBuffer:bh sourceOffset:0 toBuffer:b destinationOffset:0 size:bc*4];[upload endEncoding];[initial commit];[initial waitUntilCompleted];check(initial.status==MTLCommandBufferStatusCompleted);
  auto encode=[&](id<MTLCommandBuffer> cb,id<MTLComputePipelineState> pipeline,id<MTLBuffer> target,unsigned columns) {
    auto e=[cb computeCommandEncoder];check(e);[e setComputePipelineState:pipeline];[e setBuffer:a offset:0 atIndex:0];[e setBuffer:b offset:0 atIndex:1];[e setBuffer:target offset:0 atIndex:2];[e setBuffer:params offset:0 atIndex:3];
    [e dispatchThreadgroups:MTLSizeMake(((m+15)/16)*((n+columns-1)/columns),1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
  };
  auto download=[&](id<MTLBuffer> source) {auto cb=[queue commandBuffer];auto e=[cb blitCommandEncoder];[e copyFromBuffer:source sourceOffset:0 toBuffer:readback destinationOffset:0 size:source.length];[e endEncoding];[cb commit];[cb waitUntilCompleted];check(cb.status==MTLCommandBufferStatusCompleted);return (uint32_t*)readback.contents;};
  auto ref=[queue commandBuffer];encode(ref,reference,control,16);[ref commit];[ref waitUntilCompleted];check(ref.status==MTLCommandBufferStatusCompleted);
  std::vector<uint32_t> expected(oc);memcpy(expected.data(),download(control),oc*4);
  if(pattern!=1) for(unsigned r=0;r<m;++r)for(unsigned c=0;c<n;++c) {
    std::vector<float> products(k);for(unsigned i=0;i<k;++i)products[i]=av[size_t(r)*k+i]*bv[size_t(i)*n+c];
    for(size_t left=k;left>1;left=(left+1)/2){for(size_t i=0;i<left/2;++i)products[i]=products[i*2]+products[i*2+1];if(left%2)products[left/2]=products[left-1];}
    check(std::bit_cast<uint32_t>(products[0])==expected[size_t(r)*n+c]);
  }
  for(int round=-2;round<int(rounds);++round) {
    auto poison=[queue commandBuffer];auto fill=[poison blitCommandEncoder];[fill fillBuffer:out range:NSMakeRange(0,out.length) value:0xa5];[fill endEncoding];[poison commit];[poison waitUntilCompleted];check(poison.status==MTLCommandBufferStatusCompleted);
    auto start=std::chrono::steady_clock::now();auto cb=[queue commandBuffer];for(unsigned j=0;j<count;++j)encode(cb,compute,out,candidate?32:16);[cb commit];[cb waitUntilCompleted];
    double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();check(cb.status==MTLCommandBufferStatusCompleted);
    auto actual=download(out);check(memcmp(actual,expected.data(),oc*4)==0);for(size_t i=oc;i<oc+16;++i)check(actual[i]==0xa5a5a5a5u);
    check(memcmp(download(a),ah.contents,ac*4)==0);check(memcmp(download(b),bh.contents,bc*4)==0);
    printf("{\"round\":%d,\"ns\":%.3f,\"gpu_ns\":%.3f,\"checksum\":%llu}\n",round,ns,(cb.GPUEndTime-cb.GPUStartTime)*1e9,(unsigned long long)oc*count);
  }
}}
