// Isolated resident-kernel comparison; no production runtime or numerical change.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool ok) { if(!ok)throw std::runtime_error("Steel matrix study failed"); }
struct Params {
  int m,n,k,lda,ldb,ldd,tiles_n,tiles_m;
  int64_t batch_a,batch_b,batch_d;
  int swizzle,iterations,batch_ndim;
};
static_assert(sizeof(Params)==72);
int main(int argc,char **argv) { @autoreleasepool {
  check(argc==11);
  std::string variant=argv[2];unsigned m=std::stoul(argv[3]),k=std::stoul(argv[4]),n=std::stoul(argv[5]);
  unsigned count=std::stoul(argv[8]),rounds=std::stoul(argv[9]),dump=std::stoul(argv[10]);
  check(m && n && k && count && count<=256 && (rounds==1 || rounds==3));
  bool steel=variant.starts_with("steel");int bm=16,bn=16,bk=64,threads=256;
  std::string entry="kalandra_matmul_hardware_register16";
  if(variant=="simd") {bm=bn=bk=32;threads=128;entry="kalandra_simdgroup_matrix";}
  else if(variant=="guarded") {bm=bn=8;threads=64;entry="kalandra_matmul_hardware_tiled";}
  else if(steel) {
    if(variant=="steel64") {bm=bn=64;bk=16;threads=128;}
    else if(variant=="steel32x64") {bm=32;bn=64;bk=16;threads=64;}
    else {check(variant=="steel64x32");bm=64;bn=32;bk=32;threads=128;}
    entry=variant+"_"+((m%bm==0 && n%bn==0)?"1":"0")+"_"+(k%bk==0?"1":"0");
  } else check(variant=="baseline");
  if(variant=="baseline")check(m%16==0 && n%16==0 && k>=512 && k%64==0);
  if(variant=="simd")check(m%32==0 && n%32==0 && k%32==0);
  auto device=MTLCreateSystemDefaultDevice();check(device);NSError *error=nil;
  auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];check(library);
  auto function=[library newFunctionWithName:@(entry.c_str())];check(function);
  auto pipeline=[device newComputePipelineStateWithFunction:function error:&error];
  check(pipeline && pipeline.maxTotalThreadsPerThreadgroup>=threads && pipeline.threadExecutionWidth==32);
  auto queue=[device newCommandQueue];check(queue);
  const size_t ac=size_t(m)*k,bc=size_t(k)*n,oc=size_t(m)*n;
  auto make=[&](size_t bytes,bool shared) {auto v=[device newBufferWithLength:bytes options:shared?MTLResourceStorageModeShared:MTLResourceStorageModePrivate];check(v);return v;};
  auto ah=make(ac*4+64,true),bh=make(bc*4+64,true),a=make(ac*4+64,false),b=make(bc*4+64,false),out=make(oc*4+64,false);
  memset(ah.contents,0xa5,ah.length);memset(bh.contents,0xa5,bh.length);
  auto in=fopen(argv[6],"rb");check(in);check(fread(ah.contents,4,ac,in)==ac && fread(bh.contents,4,bc,in)==bc && fgetc(in)==EOF);fclose(in);
  auto complete=[&](id<MTLCommandBuffer> cb) {[cb commit];[cb waitUntilCompleted];check(cb.status==MTLCommandBufferStatusCompleted);};
  auto upload=[queue commandBuffer];auto blit=[upload blitCommandEncoder];
  [blit copyFromBuffer:ah sourceOffset:0 toBuffer:a destinationOffset:0 size:ah.length];[blit copyFromBuffer:bh sourceOffset:0 toBuffer:b destinationOffset:0 size:bh.length];[blit endEncoding];complete(upload);
  auto readback=make(std::max({a.length,b.length,out.length}),true);
  auto download=[&](id<MTLBuffer> src) {auto cb=[queue commandBuffer];auto enc=[cb blitCommandEncoder];[enc copyFromBuffer:src sourceOffset:0 toBuffer:readback destinationOffset:0 size:src.length];[enc endEncoding];complete(cb);return static_cast<unsigned char*>(readback.contents);};
  Params values{int(m),int(n),int(k),int(k),int(n),int(n),int((n+bn-1)/bn),int((m+bm-1)/bm),int64_t(m)*k,int64_t(k)*n,int64_t(m)*n,0,int(k/bk),0};
  auto parameters=make(steel?sizeof(values):12,true);
  if(steel)memcpy(parameters.contents,&values,sizeof(values));else {std::array shape{m,k,n};memcpy(parameters.contents,shape.data(),12);}
  auto encode=[&](id<MTLCommandBuffer> cb) {auto e=[cb computeCommandEncoder];check(e);[e setComputePipelineState:pipeline];
    [e setBuffer:a offset:0 atIndex:0];[e setBuffer:b offset:0 atIndex:1];[e setBuffer:out offset:0 atIndex:2];[e setBuffer:parameters offset:0 atIndex:3];
    const unsigned gx=(n+bn-1)/bn,gy=(m+bm-1)/bm;
    [e dispatchThreadgroups:variant=="guarded"?MTLSizeMake(gx*gy,1,1):MTLSizeMake(gx,gy,1) threadsPerThreadgroup:MTLSizeMake(threads,1,1)];[e endEncoding];};
  auto file=dump?fopen(argv[7],"wb"):nullptr;check(!dump || file);
  std::vector<unsigned char> expected;
  for(int round=-2;round<int(rounds);++round) {
    auto poison=[queue commandBuffer];auto fill=[poison blitCommandEncoder];[fill fillBuffer:out range:NSMakeRange(0,out.length) value:0xa5];[fill endEncoding];complete(poison);
    auto start=std::chrono::steady_clock::now();auto cb=[queue commandBuffer];for(unsigned i=0;i<count;++i)encode(cb);complete(cb);
    double elapsed=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
    auto bytes=download(out);for(size_t i=oc*4;i<out.length;++i)check(bytes[i]==0xa5);
    if(expected.empty())expected.assign(bytes,bytes+oc*4);else check(memcmp(bytes,expected.data(),oc*4)==0);
    if(dump)check(fwrite(bytes,4,oc,file)==oc);
    check(memcmp(download(a),ah.contents,ah.length)==0 && memcmp(download(b),bh.contents,bh.length)==0);
    printf("{\"round\":%d,\"ns\":%.3f,\"gpu_ns\":%.3f,\"checksum\":%zu,\"threads\":%d,\"static_tg_bytes\":%lu}\n",round,elapsed,(cb.GPUEndTime-cb.GPUStartTime)*1e9,oc*count,threads,(unsigned long)pipeline.staticThreadgroupMemoryLength);
  }
  if(file)check(fclose(file)==0);
}}
