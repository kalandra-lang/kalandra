#include "../metal/Metal.h"
#include "../metal/MatmulLayout.h"
#include "../metal-proof/FloatOracle.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <new>
#include <source_location>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
namespace m=kalandra::runtime::metal;
using Bits=std::uint32_t;
static_assert(m::steelMatmulVariant(512,512,2048)==3);
static_assert(m::steelMatmulVariant(64,2048,512)==7);
static_assert(m::steelMatmulVariant(65,513,65)==4);
static_assert(!m::steelMatmulVariant(63,512,512));
static_assert(!m::steelMatmulVariant(64,63,512));
static_assert(!m::steelMatmulVariant(64,512,31));
static_assert(!m::steelMatmulVariant(UINT32_MAX,UINT32_MAX,32));
static_assert(!m::steelMatmulVariant(64,UINT32_MAX,UINT32_MAX));
static void check(bool ok,std::source_location at=std::source_location::current()) {
  if(!ok)throw std::runtime_error("Steel runtime assertion line "+std::to_string(at.line()));
}
static Bits flush(Bits x) {return (x&0x7f800000u)==0?x&0x80000000u:x;}
static Bits op(Bits a,Bits b,unsigned kind) {return flush(float_proof::oracle({flush(a),flush(b)},kind));}
static Bits fma(Bits a,Bits b,Bits c) {
  llvm::APFloat x(llvm::APFloat::IEEEsingle(),llvm::APInt(32,flush(a)));
  llvm::APFloat y(llvm::APFloat::IEEEsingle(),llvm::APInt(32,flush(b)));
  llvm::APFloat z(llvm::APFloat::IEEEsingle(),llvm::APInt(32,flush(c)));
  x.fusedMultiplyAdd(y,z,llvm::APFloat::rmNearestTiesToEven);
  return flush(x.bitcastToAPInt().getZExtValue());
}
static Bits sample(unsigned i,unsigned stream,unsigned pattern) {
  if(pattern==1)return stream?0x3f800000u:0x80000000u;
  if(pattern==2)return stream?0x3f800000u:std::array{0x60ad78ecu,0x3f800000u,0xe0ad78ecu,0x3f800000u}[i%4];
  if(pattern==3)return std::array{0x7f800000u,0xff800000u,0x7fc12345u,0x00800000u,1u,0x3f800000u}[i%6];
  if(pattern==4)return stream?std::array{0x40000000u,0xc0000000u}[i%2]:0x7f7fffffu;
  std::uint32_t x=(i+stream*37+19)*2654435761u;x^=x>>13;
  return (x&0x807fffffu)|((120+x%15)<<23);
}
static void retire(TaskCore& core,GroupHandle group) {
  check(core.close(group)&&core.drain()&&core.release(group));
  const auto s=core.stats();check(!s.groups&&!s.records&&!s.externalPending&&!s.retainedBytes&&!s.retainedOwners);
}
static m::Buffer observe(TaskCore& core,TaskHandle task) {
  Invocation context;alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task,m::Runtime::bufferLayout(),context,slot));
  auto value=std::launder(reinterpret_cast<m::Buffer*>(slot));auto result=*value;value->~Buffer();return result;
}
static m::Buffer upload(m::Runtime& r,std::span<const Bits> bits,std::array<std::int64_t,2> shape) {
  auto host=r.hostTensor(std::as_bytes(bits),shape,m::Format::Float32);check(bool(host));
  auto core=TaskCore::create();check(bool(core));auto g=core->openRoot();
  auto task=r.upload(*core,g,*host,*r.device());check(task&&core->drain());
  auto result=observe(*core,task);retire(*core,g);return result;
}
static std::vector<Bits> download(m::Runtime& r,m::Buffer value) {
  auto core=TaskCore::create();auto g=core->openRoot();auto task=r.download(*core,g,value);
  check(task&&core->drain());auto host=observe(*core,task);auto bytes=host.hostBytes();check(bool(bytes));
  std::vector<Bits> bits(bytes->size()/4);if(!bits.empty())memcpy(bits.data(),bytes->data(),bytes->size());
  retire(*core,g);return bits;
}
int main(int argc,char** argv) {
  check(argc==2||argc==3);const std::string directory=argv[1];std::uint64_t cells=0,cases=0;
  for(bool fixed:{true,false}) {
    m::Options options;options.fixedTreeMatmul=fixed;options.profile=true;
    auto made=m::Runtime::create(options);check(bool(made));auto& r=**made;auto device=r.device();check(bool(device));
    // Fail each preparation allocation, including the newly added profiles.
    // Partial pipeline sets must never publish a usable kernel.
    for(bool fused:{false,true}) {
      bool loaded=false;
      for(unsigned fail=1;fail<=128&&!loaded;++fail) {
        r.failAllocationAt(fail);
        auto attempted=fused?r.loadElementwiseKernel(directory+"/kernel0-post.metallib",*device,{4,1},std::array<std::uint32_t,2>{0,2}):r.loadMatmulKernel(directory+"/kernel0.metallib",*device);
        loaded=bool(attempted);
        if(!loaded)check(attempted.error()==m::Error::Allocation);
        check(!r.stats().bytes&&!r.stats().inFlight&&!r.stats().argumentBytes);
      }
      r.failAllocationAt(0);check(loaded);
    }
    auto matrix=r.loadMatmulKernel(directory+"/kernel0.metallib",*device);check(bool(matrix));
    auto post=r.loadElementwiseKernel(directory+"/kernel0-post.metallib",*device,{4,1},std::array<std::uint32_t,2>{0,2});check(bool(post));
    for(auto shape:{std::array<unsigned,3>{64,512,32},{65,513,65},{65,64,33},{129,129,97},{64,67,32},
                    {128,96,64},{512,256,512},{512,64,2048},{513,65,2049},{513,64,2049},
                    {512,65,2048},{3,7,5},{1,512,512},{63,64,32},
                    {0,64,32},{64,0,32},{64,64,0}}) {
      auto [rows,k,n]=shape;
      // Exercise every large-tile alignment entry with finite and basis inputs.
      // All IEEE/cancellation patterns also cover its aligned entry and the
      // smaller tile's guarded entries, without multiplying huge readbacks.
      const bool largeTail=rows>=512&&(rows%64||k%16||n%64);
      for(unsigned pattern=0;pattern<6;++pattern) {
        if(largeTail&&pattern!=0&&pattern!=5)continue;
        std::vector<Bits> a(size_t(rows)*k),b(size_t(k)*n),bias(size_t(rows)*n);
        for(unsigned row=0;row<rows;++row)for(unsigned i=0;i<k;++i)
          a[size_t(row)*k+i]=pattern==5?std::bit_cast<Bits>(float(row*k+i+1)/1024):sample(i+(row%4)*7,0,pattern);
        for(unsigned i=0;i<k;++i)for(unsigned col=0;col<n;++col)
          b[size_t(i)*n+col]=pattern==5?(i==col%k?std::bit_cast<Bits>(float(col+1)/1024):0):sample(i+(col%4)*11,1,pattern);
        // Unlike the repeated matrix rows/columns, captures vary at every
        // physical position to expose epilogue row/column addressing errors.
        for(size_t i=0;i<bias.size();++i)bias[i]=sample(unsigned(i),2,0);
        std::array<Bits,16> reference{};
        // Random/IEEE cases repeat four rows/columns to bound APFloat cost.
        // The scaled basis case uses globally distinct rows and columns with
        // one nonzero product per output, exposing tile/base/tail addressing
        // errors with an independent one-product oracle. Every cell is checked.
        const bool optimized=!fixed&&device->name()=="Apple M2 Max"&&m::steelMatmulVariant(rows,k,n).has_value();
        for(unsigned row=0;pattern!=5&&row<std::min(rows,4u);++row)for(unsigned col=0;col<std::min(n,4u);++col) {
          Bits acc=0;std::vector<Bits> products(k);
          for(unsigned i=0;i<k;++i) {
            if(optimized)acc=fma(a[size_t(row)*k+i],b[size_t(i)*n+col],acc);
            else products[i]=op(a[size_t(row)*k+i],b[size_t(i)*n+col],1);
          }
          if(!optimized) {
            for(size_t size=k;size>1;size=(size+1)/2) {
              for(size_t i=0;i<size/2;++i)products[i]=op(products[2*i],products[2*i+1],0);
              if(size%2)products[size/2]=products[size-1];
            }
            acc=k?products[0]:0;
          }
          reference[row*4+col]=acc;
        }
        {
          auto ga=upload(r,a,{rows,k}),gb=upload(r,b,{k,n}),gc=upload(r,bias,{rows,n});
          const auto baseline=r.stats().bytes;
          for(bool fused:{false,true}) {
            auto core=TaskCore::create();auto g=core->openRoot();auto before=r.stats().dispatches;
            auto task=fused?r.matmul(*core,g,*matrix,ga,gb,*post,std::array{ga,ga,gc,gb},std::array{1.5f}):r.matmul(*core,g,*matrix,ga,gb);
            check(task&&core->drain());auto result=observe(*core,task),cached=observe(*core,task);
            check(result.shape()[0]==rows&&result.shape()[1]==n&&cached.bytes()==result.bytes());
            if(rows&&n&&k&&optimized) {
              const auto dispatches=r.stats().dispatches-before;
              // This legacy untyped API retains its conservative quad-layout
              // post-map boundary. Typed language graphs use their own layout.
              const bool legacyQuad=fused&&rows==512&&k==256&&n==512;
              check(dispatches==1||(legacyQuad&&dispatches==2));
            }
            auto actual=download(r,result);check(actual.size()==size_t(rows)*n);
            for(unsigned row=0;row<rows;++row)for(unsigned col=0;col<n;++col) {
              auto expected=pattern==5?(k?op(a[size_t(row)*k+col%k],std::bit_cast<Bits>(float(col+1)/1024),1):0):reference[(row%4)*4+col%4];
              if(fused)expected=op(op(expected,bias[size_t(row)*n+col],0),std::bit_cast<Bits>(1.5f),1);
              if(!float_proof::equal(actual[size_t(row)*n+col],expected)) {
                fprintf(stderr,"fixed=%d shape=%u,%u,%u pattern=%u fused=%d cell=%u,%u actual=%08x expected=%08x\n",fixed,rows,k,n,pattern,fused,row,col,actual[size_t(row)*n+col],expected);
                check(false);
              }
              ++cells;
            }
            result={};cached={};retire(*core,g);check(r.stats().bytes==baseline);++cases;
          }
          check(download(r,ga)==a&&download(r,gb)==b&&download(r,gc)==bias);
        }
        check(!r.stats().bytes&&!r.stats().inFlight&&!r.stats().argumentBytes);
      }
    }
    // Pending completion retains backing through cancellation/deadlines and
    // device faults; no cancelled/failed output can be observed.
    std::vector<Bits> values(64*64,0x3f800000u);
    auto ga=upload(r,values,{64,64}),gb=upload(r,values,{64,64});
    const auto baseline=r.stats().bytes;
    for(bool fused:{false,true}) for(unsigned scenario=0;scenario<4;++scenario) {
      auto core=TaskCore::create();auto g=core->openRoot();r.holdCompletions(true);
      if(scenario>=2)r.failNextCompletion();
      auto task=fused?r.matmul(*core,g,*matrix,ga,gb,*post,std::array{ga,ga,ga,gb},std::array{1.0f}):r.matmul(*core,g,*matrix,ga,gb);
      check(task&&core->step());
      check(core->stats().externalPending==1&&r.stats().bytes>baseline);
      if(scenario<2||scenario==3)check(core->cancel(g,scenario==1));
      check(!core->settled(g));r.holdCompletions(false);check(core->drain());
      // Cancellation recorded before resumption wins over an as-yet unlatched
      // completion failure. A fault already latched by execution stays first.
      check(core->failure(task).kind==(scenario==2?TaskFailure::Fault:TaskFailure::Cancelled));
      if(scenario==2) {
        check(core->failure(task).code==std::int64_t(m::Error::Device));
        core->cancel(g);
        check(core->failure(task).kind==TaskFailure::Fault&&core->failure(g).kind==TaskFailure::Fault);
      }
      Invocation context;alignas(m::Buffer) std::byte untouched[sizeof(m::Buffer)];
      memset(untouched,0xa5,sizeof(untouched));check(!core->observe(task,m::Runtime::bufferLayout(),context,untouched));
      for(auto byte:untouched)check(byte==std::byte{0xa5});
      retire(*core,g);check(r.stats().bytes==baseline);
    }
    bool succeeded=false;
    for(unsigned fail=1;fail<=64&&!succeeded;++fail) {
      r.failAllocationAt(fail);auto core=TaskCore::create();auto g=core->openRoot();
      auto task=r.matmul(*core,g,*matrix,ga,gb);check(task&&core->drain());
      succeeded=core->failure(task).kind==TaskFailure::None;
      if(!succeeded)check(core->failure(task).kind==TaskFailure::Fault);
      retire(*core,g);check(r.stats().bytes==baseline);
    }
    r.failAllocationAt(0);check(succeeded);
    if(!fixed&&argc==3) {
      // Mixing modules must not let an optimized epilogue relax a matrix that
      // was explicitly compiled fixed-tree (or came from an older bundle).
      auto legacy=r.loadMatmulKernel(std::string(argv[2])+"/kernel0.metallib",*device);
      check(bool(legacy));
      {
        std::vector<Bits> left(64*512),right(512*32,0x3f800000u),bias(64*32,0);
        for(size_t i=0;i<left.size();++i)left[i]=sample(i%512,0,2);
        auto a=upload(r,left,{64,512}),b=upload(r,right,{512,32}),c=upload(r,bias,{64,32});
        auto core=TaskCore::create();auto g=core->openRoot();
        auto task=r.matmul(*core,g,*legacy,a,b,*post,std::array{a,a,c,b},std::array{1.0f});
        check(task&&core->drain());auto result=observe(*core,task);
        retire(*core,g);core.reset();
        auto actual=download(r,result);check(actual.size()==64*32);
        for(auto value:actual)check(value==0); // Fused accumulation would yield 1.
        cells+=actual.size();++cases;
      }
      check(r.stats().bytes==baseline);
    }
  }
  printf("Steel runtime: %llu cases, %llu output cells; fixed-tree/fused arithmetic, post-map fusion, tails/empties, cached ownership, cancellation/deadlines, faults and allocation cleanup\n",(unsigned long long)cases,(unsigned long long)cells);
}
