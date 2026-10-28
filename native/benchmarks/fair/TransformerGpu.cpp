#include "TransformerBlock.h"
#include "../../metal/Metal.h"
#include <new>
using namespace kalandra::runtime;
namespace m=kalandra::runtime::metal;
static m::Buffer observe(TaskCore &core,TaskHandle task) {
  Invocation context;alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  ffn::check(core.observe(task,m::Runtime::bufferLayout(),context,slot));
  auto *value=std::launder(reinterpret_cast<m::Buffer*>(slot));auto result=*value;value->~Buffer();return result;
}
static void retire(TaskCore &core,GroupHandle group) {
  ffn::check(core.close(group) && core.drain() && core.release(group));
  auto s=core.stats();ffn::check(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
// Generated from the actual compiler's host.cpp. The runner validates source,
// instruction sequence, scalar captures, library names, and artifact hashes.
#include "TransformerProgram.h"
int main(int argc,char**argv) {
  ffn::check(argc==9);
  const unsigned rows=std::stoul(argv[2]),d=std::stoul(argv[3]),count=std::stoul(argv[4]),rounds=std::stoul(argv[5]),mode=std::stoul(argv[6]),dump=std::stoul(argv[7]),pattern=std::stoul(argv[8]);
  ffn::check(rows && d && rows<=512 && d<=2048 && mode<=2);
  auto made=m::Runtime::create();ffn::check(bool(made));auto&r=**made;auto device=r.device();ffn::check(bool(device));
  auto core=TaskCore::create();ffn::check(bool(core));Program program(r,argv[1]);
  const auto preparedPacks=r.stats().argumentPacks,preparedBytes=r.stats().argumentBytes;
  transformer::Inputs data(rows,d,pattern);
  {
    std::array<m::Buffer,7> host,resident;
    auto upload=[&](m::Buffer buffer) {auto g=core->openRoot();auto t=r.upload(*core,g,buffer,*device);ffn::check(t && core->drain());auto result=observe(*core,t);retire(*core,g);return result;};
    for(unsigned i=0;i<7;++i) {
      auto h=r.hostTensor(std::as_bytes(std::span(data.values[i])),data.shapes[i],m::Format::Float32);ffn::check(bool(h));host[i]=*h;
      if(!mode || (mode==1 && i)) resident[i]=upload(host[i]);
    }
    const auto baseline=r.stats().bytes;
    auto call=[&](bool save) {
      auto input=resident;
      for(unsigned i=0;i<7;++i) if(mode==2 || (mode==1 && !i)) input[i]=upload(host[i]);
      auto group=core->openRoot();auto task=program.launch(r,*core,group,input,transformer::epsilon,data.scale);
      ffn::check(task && core->drain() && core->failure(task).kind==TaskFailure::None);
      auto out=observe(*core,task);ffn::check(out.shape().size()==2 && out.shape()[0]==rows && out.shape()[1]==d && out.bytes()==rows*d*4ull);
      if(save) {
        auto t=r.download(*core,group,out);ffn::check(t && core->drain());auto h=observe(*core,t);auto bytes=h.hostBytes();ffn::check(bool(bytes));
        std::vector<float> cells(rows*d);ffn::check(bytes->size()==cells.size()*4);std::memcpy(cells.data(),bytes->data(),bytes->size());ffn::dump(cells,ffn::sum(cells));
      }
      out={};retire(*core,group);return double(rows)*d*4;
    };
    if(dump) call(true);
    else for(int round=-2;round<int(rounds);++round) {
      auto before=r.stats();double checksum=0;auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<count;++i) checksum+=call(false);
      const double ns=ffn::elapsed(start);auto after=r.stats();
      ffn::check(after.bytes==baseline && !after.inFlight && after.argumentPacks==preparedPacks && after.argumentBytes==preparedBytes);
      std::printf("{\"preparation_ns\":%.0f,\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g,\"submissions\":%llu,\"dispatches\":%llu,\"gpu_ns\":%llu,\"encoding_ns\":%llu,\"attempts\":%llu,\"resident_bytes\":%llu,\"peak_bytes\":%llu}\n",program.preparationNanoseconds,round,ns,checksum,(unsigned long long)(after.submissions-before.submissions),(unsigned long long)(after.dispatches-before.dispatches),(unsigned long long)(after.gpuNanoseconds-before.gpuNanoseconds),(unsigned long long)(after.encodingNanoseconds-before.encodingNanoseconds),(unsigned long long)(after.attempts-before.attempts),(unsigned long long)baseline,(unsigned long long)after.peakBytes);
    }
  }
  auto s=r.stats();ffn::check(!s.bytes && !s.hostBackings && !s.deviceBackings && !s.metalBuffers && !s.stagingBuffers);
}
