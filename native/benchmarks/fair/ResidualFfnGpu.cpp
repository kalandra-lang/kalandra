#include "ResidualFfn.h"
#include "../../metal/Metal.h"
#include <array>
#include <cstring>
#include <new>
using namespace kalandra;
using namespace kalandra::runtime;
namespace m=kalandra::runtime::metal;
static m::Buffer observe(TaskCore& core,TaskHandle task) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  ffn::check(core.observe(task,m::Runtime::bufferLayout(),ctx,slot));
  auto* value=std::launder(reinterpret_cast<m::Buffer*>(slot));
  auto result=*value;
  value->~Buffer();
  return result;
}
static void retire(TaskCore& core,GroupHandle group) {
  ffn::check(core.close(group) && core.drain() && core.release(group));
  const auto s=core.stats();
  ffn::check(!s.groups && !s.records && !s.retainedOwners && !s.retainedBytes && !s.externalPending);
}
static m::Buffer transfer(m::Runtime& r,TaskCore& core,m::Buffer input,m::Device device,bool download=false) {
  const auto group=core.openRoot();
  auto task=download?r.download(core,group,input):r.upload(core,group,input,device);
  ffn::check(task && core.drain());
  auto result=observe(core,task);
  retire(core,group);
  return result;
}
int main(int argc,char** argv) {
  if(argc!=9) return 2;
  const std::string directory=argv[1];
  const unsigned rows=std::stoul(argv[2]),d=std::stoul(argv[3]),count=std::stoul(argv[4]),
    rounds=std::stoul(argv[5]),mode=std::stoul(argv[6]),pattern=std::stoul(argv[7]),dump=std::stoul(argv[8]);
  if(!rows || rows>128 || !d || d>2048 || !count || !rounds || mode>2 || pattern>1) return 2;
  m::Options options;
  options.profile=std::getenv("KALANDRA_GPU_PROFILE")!=nullptr;
  options.maxBytes=std::max(options.maxBytes,std::uint64_t(128)*d*d+std::uint64_t(rows)*d*128);
  auto made=m::Runtime::create(options);
  ffn::check(bool(made));auto& r=**made;
  auto device=r.device();ffn::check(bool(device));
  TaskLimits limits;limits.workers=1;
  auto core=TaskCore::create(limits);ffn::check(bool(core));
  auto matrix=r.loadMatmulKernel(directory+"/kernel0.metallib",*device);
  auto reduce=r.loadReductionKernel(directory+"/kernel0-reduce.metallib",*device);
  auto relu=r.loadElementwiseKernel(directory+"/kernel0-map1.metallib",*device,{1,0});
  auto residual=r.loadElementwiseKernel(directory+"/kernel0-map3.metallib",*device,{2,0});
  ffn::check(matrix && reduce && relu && residual);
  // Exactly the graph emitted in bundle/host.cpp; the runner asserts this too.
  const std::array<m::MatrixProduct,4> products{{{0,1},{3,3},{4,2},{0,5}}};
  const std::array<m::Kernel,4> maps{{{},*relu,{},*residual}};
  ffn::Inputs data(rows,d,pattern);
  {
    auto hx=r.hostTensor(std::as_bytes(std::span(data.x)),std::array<std::int64_t,2>{rows,d},m::Format::Float32);
    auto hu=r.hostTensor(std::as_bytes(std::span(data.up)),std::array<std::int64_t,2>{d,4*d},m::Format::Float32);
    auto hd=r.hostTensor(std::as_bytes(std::span(data.down)),std::array<std::int64_t,2>{4*d,d},m::Format::Float32);
    ffn::check(hx && hu && hd);
    m::Buffer x,up,down;
    if(mode!=2) {
      up=transfer(r,*core,*hu,*device);down=transfer(r,*core,*hd,*device);
      if(mode==0) x=transfer(r,*core,*hx,*device);
    }
    const auto baseline=r.stats().bytes;
    auto call=[&]() {
      auto a=mode?transfer(r,*core,*hx,*device):x;
      auto b=mode==2?transfer(r,*core,*hu,*device):up;
      auto c=mode==2?transfer(r,*core,*hd,*device):down;
      auto group=core->openRoot();
      auto task=dump?r.matmulGraph(*core,group,*matrix,std::array{a,b,c},products,maps)
                    :r.matmulGraphReduceSum(*core,group,*matrix,std::array{a,b,c},products,*reduce,{}, {},maps);
      ffn::check(task && core->drain());
      float value=0;
      if(dump) {
        auto output=observe(*core,task);
        retire(*core,group);
        auto host=transfer(r,*core,output,*device,true);
        auto bytes=host.hostBytes();ffn::check(bool(bytes));
        std::vector<float> cells(rows*d);
        ffn::check(bytes->size()==cells.size()*4);
        std::memcpy(cells.data(),bytes->data(),bytes->size());
        value=ffn::sum(cells);ffn::dump(cells,value);
        return value;
      } else {
        Invocation ctx;
        ffn::check(core->observe(task,m::Runtime::scalarLayout(),ctx,&value));
      }
      retire(*core,group);
      return value;
    };
    if(dump) call();
    else for(int round=-2;round<int(rounds);++round) {
      const auto before=r.stats();
      double checksum=0;
      auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<count;++i) checksum+=call();
      const auto ns=ffn::elapsed(start);const auto after=r.stats();
      ffn::check(after.bytes==baseline && !after.inFlight && !after.stagingBuffers && !after.argumentPacks && !after.argumentBytes);
      std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"submissions\":%llu,\"completions\":%llu,\"dispatches\":%llu,\"gpu_ns\":%llu,\"encoding_ns\":%llu,\"peak_bytes\":%llu,\"resident_bytes\":%llu}\n",
        round,ns,checksum,(unsigned long long)(after.submissions-before.submissions),(unsigned long long)(after.completions-before.completions),
        (unsigned long long)(after.dispatches-before.dispatches),(unsigned long long)(after.gpuNanoseconds-before.gpuNanoseconds),
        (unsigned long long)(after.encodingNanoseconds-before.encodingNanoseconds),(unsigned long long)after.peakBytes,(unsigned long long)baseline);
    }
  }
  const auto s=r.stats();
  ffn::check(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings);
}
