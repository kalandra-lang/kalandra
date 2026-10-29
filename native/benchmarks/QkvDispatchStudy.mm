// Isolated three-projection diagnostic; not a language dispatch policy.
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
static void check(bool value) { if (!value) throw std::runtime_error("QKV dispatch check failed"); }
int main(int argc,char**argv) { @autoreleasepool {
  check(argc==9);
  std::string mode=argv[2];check(mode=="baseline" || mode=="current" || mode=="serial");
  unsigned m=std::stoul(argv[3]),k=std::stoul(argv[4]),n=std::stoul(argv[5]),
    count=std::stoul(argv[6]),rounds=std::stoul(argv[7]),pattern=std::stoul(argv[8]);
  check(m && n && k>=512 && k<=4096 && k%64==0 && count && count<=256 && pattern<=2);
  auto device=MTLCreateSystemDefaultDevice();check(device);NSError*error=nil;
  auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];check(library);
  auto make=[&](const char*name) {auto f=[library newFunctionWithName:@(name)];check(f);auto p=[device newComputePipelineStateWithFunction:f error:&error];check(p && p.maxTotalThreadsPerThreadgroup>=256);return p;};
  auto ordinary=make("kalandra_matmul_hardware_register16"),batched=make("kalandra_matmul_hardware_qkv");
  auto queue=[device newCommandQueue];check(queue);
  auto buffer=[&](size_t bytes,bool shared=false) {auto b=[device newBufferWithLength:bytes options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];check(b);return b;};
  size_t ac=size_t(m)*k,bc=size_t(k)*n,oc=size_t(m)*n;
  auto ah=buffer(ac*4,true),a=buffer(ac*4),params=buffer(12,true),readback=buffer(std::max({ac,bc,oc})*4+64,true);
  std::array<id<MTLBuffer>,3> bh,b,out,control;
  for(unsigned p=0;p<3;++p) {bh[p]=buffer(bc*4,true);b[p]=buffer(bc*4);out[p]=buffer(oc*4+64);control[p]=buffer(oc*4+64);}
  std::array shape{m,k,n};memcpy(params.contents,shape.data(),12);
  float*av=(float*)ah.contents;
  constexpr std::array<uint32_t,12> ieee{0,0x80000000,0x3f800000,0xbf800000,1,0x80000001,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0xffc12345};
  for(size_t i=0;i<ac;++i) av[i]=pattern==1?std::bit_cast<float>(ieee[(i+i/k)%ieee.size()]):pattern==2?-0.0f:float((int(i%17)-8)*0.134567+(i%29==0?10000.0003:0.00043));
  for(unsigned p=0;p<3;++p) for(size_t i=0;i<bc;++i) ((float*)bh[p].contents)[i]=pattern==1?std::bit_cast<float>(ieee[(i*7+i/n+p*3)%ieee.size()]):pattern==2?(p==1?-1.0f:1.0f):float((int((i+p*3)%13)-6)*0.173219+(i%31==0?-0.000023:0.00019));
  auto finish=[&](id<MTLCommandBuffer>cb) {[cb commit];[cb waitUntilCompleted];check(cb.status==MTLCommandBufferStatusCompleted);};
  auto initial=[queue commandBuffer];auto upload=[initial blitCommandEncoder];
  [upload copyFromBuffer:ah sourceOffset:0 toBuffer:a destinationOffset:0 size:ac*4];
  for(unsigned p=0;p<3;++p) [upload copyFromBuffer:bh[p] sourceOffset:0 toBuffer:b[p] destinationOffset:0 size:bc*4];
  [upload endEncoding];finish(initial);
  auto encode=[&](id<MTLComputeCommandEncoder>e,unsigned p,id<MTLBuffer>target,bool batch) {
    [e setComputePipelineState:batch?batched:ordinary];[e setBuffer:a offset:0 atIndex:0];[e setBuffer:b[p] offset:0 atIndex:1];[e setBuffer:target offset:0 atIndex:2];[e setBuffer:params offset:0 atIndex:3];
    if(batch) { [e setBuffer:b[1] offset:0 atIndex:4];[e setBuffer:out[1] offset:0 atIndex:5];[e setBuffer:b[2] offset:0 atIndex:6];[e setBuffer:out[2] offset:0 atIndex:7]; }
    [e dispatchThreadgroups:MTLSizeMake(((m+15)/16)*((n+15)/16),batch?3:1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];
  };
  auto download=[&](id<MTLBuffer>source) {auto cb=[queue commandBuffer];auto e=[cb blitCommandEncoder];[e copyFromBuffer:source sourceOffset:0 toBuffer:readback destinationOffset:0 size:source.length];[e endEncoding];finish(cb);return (uint32_t*)readback.contents;};
  std::array<std::vector<uint32_t>,3> expected;
  for(unsigned p=0;p<3;++p) {
    auto cb=[queue commandBuffer];auto e=[cb computeCommandEncoder];encode(e,p,control[p],false);[e endEncoding];finish(cb);
    expected[p].resize(oc);memcpy(expected[p].data(),download(control[p]),oc*4);
    if(pattern!=1) for(unsigned r=0;r<m;++r)for(unsigned c=0;c<n;++c) {
      std::vector<float> products(k);for(unsigned i=0;i<k;++i)products[i]=av[size_t(r)*k+i]*((float*)bh[p].contents)[size_t(i)*n+c];
      for(size_t left=k;left>1;left=(left+1)/2) {for(size_t i=0;i<left/2;++i)products[i]=products[i*2]+products[i*2+1];if(left%2)products[left/2]=products[left-1];}
      check(std::bit_cast<uint32_t>(products[0])==expected[p][size_t(r)*n+c]);
    }
  }
  for(int round=-2;round<int(rounds);++round) {
    auto poison=[queue commandBuffer];auto fill=[poison blitCommandEncoder];for(auto o:out)[fill fillBuffer:o range:NSMakeRange(0,o.length) value:0xa5];[fill endEncoding];finish(poison);
    auto start=std::chrono::steady_clock::now();auto cb=[queue commandBuffer];
    for(unsigned j=0;j<count;++j) {
      if(mode=="baseline") {for(unsigned p=0;p<3;++p){auto e=[cb computeCommandEncoder];encode(e,p,out[p],false);[e endEncoding];}}
      else {auto e=[cb computeCommandEncoder];if(mode=="current")encode(e,0,out[0],true);else for(unsigned p=0;p<3;++p)encode(e,p,out[p],false);[e endEncoding];}
    }
    finish(cb);double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
    for(unsigned p=0;p<3;++p){auto actual=download(out[p]);check(memcmp(actual,expected[p].data(),oc*4)==0);for(size_t i=oc;i<oc+16;++i)check(actual[i]==0xa5a5a5a5u);check(memcmp(download(b[p]),bh[p].contents,bc*4)==0);}
    check(memcmp(download(a),ah.contents,ac*4)==0);
    printf("{\"round\":%d,\"ns\":%.3f,\"gpu_ns\":%.3f,\"checksum\":%llu}\n",round,ns,(cb.GPUEndTime-cb.GPUStartTime)*1e9,(unsigned long long)oc*3*count);
  }
}}
