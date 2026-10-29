// Isolated screen: transpose+ordinary matmul versus direct A*B^T. Not a language result.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../metal/MatmulLayout.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool b) { if(!b) throw std::runtime_error("transpose matrix screen failed"); }
int main(int argc,char**argv) { @autoreleasepool {
 check(argc==8);std::string variant=argv[2];
 unsigned m=std::stoul(argv[3]),k=std::stoul(argv[4]),n=std::stoul(argv[5]),count=std::stoul(argv[6]),rounds=std::stoul(argv[7]);
 check(m&&k&&n&&count&&count<=256);
 auto device=MTLCreateSystemDefaultDevice();check(device);NSError*error=nil;
 auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];check(library);
 auto pipeline=[&](const char*name) {auto f=[library newFunctionWithName:@(name)];check(f);auto p=[device newComputePipelineStateWithFunction:f error:&error];check(p);return p;};
 auto transpose=pipeline("transpose_input");
 unsigned threads=256,groups=(m*n+7)/8;
 std::string entry="direct_nt";
 if(variant=="baseline") {
  using namespace kalandra::runtime::metal;
  bool skinny=useSkinnyMatmul(m,k,n),reg=useRegisterMatmul(m,k,n),wide=!reg&&m>=16&&n>=8&&k>=512&&k%64==0;
  entry=skinny?"kalandra_matmul_hardware_skinny32":reg?"kalandra_matmul_hardware_register16":wide?"kalandra_matmul_hardware_block64":k>=256&&k%32==0?"kalandra_matmul_hardware_block32":"kalandra_matmul_hardware_tiled";
  threads=skinny||reg?256:wide?128:64;
  groups=skinny?m*((n+31)/32):((m+(threads==64?7:15))/(threads==64?8:16))*((n+(reg?15:7))/(reg?16:8));
 } else check(variant=="current");
 auto compute=pipeline(entry.c_str());check(compute.threadExecutionWidth==32&&compute.maxTotalThreadsPerThreadgroup>=threads);
 auto queue=[device newCommandQueue];check(queue);
 auto buffer=[&](size_t bytes,bool shared=false){auto b=[device newBufferWithLength:bytes options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];check(b);return b;};
 const size_t ac=size_t(m)*k,bc=size_t(n)*k,oc=size_t(m)*n;
 auto ah=buffer(ac*4,true),bh=buffer(bc*4,true),a=buffer(ac*4),b=buffer(bc*4),bt=buffer(bc*4),out=buffer(oc*4+64),readback=buffer(std::max({ac,bc,oc})*4+64,true),params=buffer(12,true);
 std::array shape{m,k,n};memcpy(params.contents,shape.data(),12);
 auto av=(float*)ah.contents,bv=(float*)bh.contents;
 for(size_t i=0;i<ac;++i)av[i]=float(int(i%17)-8)/8;
 for(size_t i=0;i<bc;++i)bv[i]=float(int(i%13)-6)/8;
 auto initial=[queue commandBuffer];auto upload=[initial blitCommandEncoder];
 [upload copyFromBuffer:ah sourceOffset:0 toBuffer:a destinationOffset:0 size:ac*4];
 [upload copyFromBuffer:bh sourceOffset:0 toBuffer:b destinationOffset:0 size:bc*4];[upload endEncoding];[initial commit];[initial waitUntilCompleted];check(initial.status==MTLCommandBufferStatusCompleted);
 std::vector<uint32_t> expected(oc);
 for(unsigned r=0;r<m;++r)for(unsigned c=0;c<n;++c){
  std::vector<float> products(k);for(unsigned i=0;i<k;++i)products[i]=av[size_t(r)*k+i]*bv[size_t(c)*k+i];
  for(size_t left=k;left>1;left=(left+1)/2){for(size_t i=0;i<left/2;++i)products[i]=products[i*2]+products[i*2+1];if(left%2)products[left/2]=products[left-1];}
  expected[size_t(r)*n+c]=std::bit_cast<uint32_t>(products[0]);
 }
 auto download=[&](id<MTLBuffer> source){auto cb=[queue commandBuffer];auto e=[cb blitCommandEncoder];[e copyFromBuffer:source sourceOffset:0 toBuffer:readback destinationOffset:0 size:source.length];[e endEncoding];[cb commit];[cb waitUntilCompleted];check(cb.status==MTLCommandBufferStatusCompleted);return (uint32_t*)readback.contents;};
 for(int round=-2;round<int(rounds);++round){
  auto poison=[queue commandBuffer];auto fill=[poison blitCommandEncoder];[fill fillBuffer:out range:NSMakeRange(0,out.length) value:0xa5];[fill endEncoding];[poison commit];[poison waitUntilCompleted];check(poison.status==MTLCommandBufferStatusCompleted);
  auto start=std::chrono::steady_clock::now();auto cb=[queue commandBuffer];
  for(unsigned j=0;j<count;++j){
   if(variant=="baseline"){auto e=[cb computeCommandEncoder];[e setComputePipelineState:transpose];[e setBuffer:b offset:0 atIndex:0];[e setBuffer:bt offset:0 atIndex:1];[e setBuffer:params offset:0 atIndex:2];[e dispatchThreadgroups:MTLSizeMake((bc+255)/256,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];}
   auto e=[cb computeCommandEncoder];[e setComputePipelineState:compute];[e setBuffer:a offset:0 atIndex:0];[e setBuffer:variant=="baseline"?bt:b offset:0 atIndex:1];[e setBuffer:out offset:0 atIndex:2];[e setBuffer:params offset:0 atIndex:3];[e dispatchThreadgroups:MTLSizeMake(groups,1,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];[e endEncoding];
  }
  [cb commit];[cb waitUntilCompleted];double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();check(cb.status==MTLCommandBufferStatusCompleted);
  auto actual=download(out);check(memcmp(actual,expected.data(),oc*4)==0);for(size_t i=oc;i<oc+16;++i)check(actual[i]==0xa5a5a5a5u);
  check(memcmp(download(a),ah.contents,ac*4)==0);check(memcmp(download(b),bh.contents,bc*4)==0);
  printf("{\"round\":%d,\"ns\":%.3f,\"gpu_ns\":%.3f,\"checksum\":%llu}\n",round,ns,(cb.GPUEndTime-cb.GPUStartTime)*1e9,(unsigned long long)oc*count);
 }
}}
