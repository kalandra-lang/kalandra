#include "../benchmarks/fair/TransformerBlock.h"
#include "../metal/Metal.h"
#include <new>
#include <thread>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Prepared program check %d: %s\n", __LINE__, #x);                       \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
#include "LegacyProgram.h"
#include "TransformerProgram.h"
static void retire(TaskCore &c, GroupHandle g) {
  CHECK(c.close(g) && c.drain() && c.release(g));
  auto s = c.stats();
  CHECK(!s.groups && !s.records && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
static m::Buffer observe(TaskCore &c, TaskHandle t) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(c.observe(t, m::Runtime::bufferLayout(), ctx, slot));
  auto *v = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto b = *v;
  v->~Buffer();
  return b;
}
static std::vector<std::uint32_t> download(m::Runtime &r, TaskCore &c, GroupHandle g, m::Buffer b) {
  auto t = r.download(c, g, b);
  CHECK(t && c.drain());
  auto h = observe(c, t);
  auto bytes = h.hostBytes();
  CHECK(bytes);
  std::vector<std::uint32_t> result(bytes->size() / 4);
  std::memcpy(result.data(), bytes->data(), bytes->size());
  return result;
}

// Benchmark-only hooks pause the CPU builder after its first commit. No
// production API or environment-variable gate is added.
extern "C" void klOverlapPause(bool);
extern "C" bool klOverlapPaused();
extern "C" void klOverlapCallbackPause(bool);
extern "C" bool klOverlapCallbackPaused();
template<class F> static void waitFor(F done) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(!done()) { CHECK(std::chrono::steady_clock::now()<end); std::this_thread::yield(); }
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  m::Options options; options.profile=true;
  auto made=m::Runtime::create(options); CHECK(made); auto &r=**made;
  {
    Program program(r,argv[1]); LegacyProgram legacy(r,argv[1]);
    transformer::Inputs data(64,512,1);
    std::array<m::Buffer,7> input;
    for(unsigned i=0;i<7;++i) {
      auto c=TaskCore::create(); auto g=c->openRoot();
      auto h=r.hostTensor(std::as_bytes(std::span(data.values[i])),data.shapes[i],m::Format::Float32); CHECK(h);
      auto t=r.upload(*c,g,*h,*r.device()); CHECK(t&&c->drain()); input[i]=observe(*c,t); retire(*c,g);
    }
    std::vector<std::uint32_t> expected;
    { auto c=TaskCore::create(); auto g=c->openRoot();
      auto t=legacy.launch(r,*c,g,input,transformer::epsilon,data.scale); CHECK(c->drain());
      expected=download(r,*c,g,observe(*c,t)); retire(*c,g); }
    const auto baseline=r.stats();
    for(unsigned scenario=0;scenario<6;++scenario) {
      auto c=TaskCore::create();auto g=c->openRoot();auto before=r.stats();
      klOverlapPause(true);
      klOverlapCallbackPause(scenario==5);
      if(scenario==3) r.failNextCompletion();
      auto t=program.launch(r,*c,g,input,transformer::epsilon,data.scale);CHECK(t);
      std::thread worker([&]{CHECK(c->step());});
      waitFor([]{return klOverlapPaused();});
      waitFor([&]{return r.stats().completions==before.completions+1;});
      if(scenario==5) waitFor([]{return klOverlapCallbackPaused();});
      // GPU work ended while the CPU builder is still paused. The lease and
      // owned intermediate resources must remain live until that builder ends.
      CHECK(r.stats().submissions==before.submissions+1);
      CHECK(r.stats().inFlight==1&&c->stats().externalPending==1);
      CHECK(r.stats().bytes>baseline.bytes&&r.progressionCommandReservations());
      if(scenario==1||scenario==2||scenario==5) CHECK(c->cancel(g,scenario==2));
      if(scenario==4) r.failAllocationAt(1);
      klOverlapPause(false);worker.join();
      if(scenario==5) {
        // The CPU builder has ended, but the callback has not reached its
        // final handoff. It must still pin the frame despite the GPU ACK.
        CHECK(r.stats().inFlight==1&&c->stats().externalPending==1&&!c->settled(t));
        klOverlapCallbackPause(false);
      }
      CHECK(c->drain());r.failAllocationAt(0);
      auto failure=c->failure(t);
      std::fprintf(stderr,"gate scenario %u: failure %u/%lld\n",scenario,unsigned(failure.kind),(long long)failure.code);
      if(scenario==0) {
        CHECK(failure.kind==TaskFailure::None);
        CHECK(r.stats().submissions==before.submissions+5);
        CHECK(download(r,*c,g,observe(*c,t))==expected);
      } else {
        CHECK(r.stats().submissions==before.submissions+1);
        if(scenario==1||scenario==2||scenario==5) {
          CHECK(failure.kind==TaskFailure::Cancelled);
          CHECK(c->failure(g).kind==(scenario==2?TaskFailure::DeadlineExceeded:TaskFailure::Cancelled));
        }
        else CHECK(failure.kind==TaskFailure::Fault&&failure.code==std::int64_t(scenario==3?m::Error::Device:m::Error::Allocation));
      }
      retire(*c,g);auto after=r.stats();
      CHECK(after.bytes==baseline.bytes&&!after.inFlight&&!after.stagingBuffers);
      CHECK(after.argumentBytes==baseline.argumentBytes&&after.argumentPacks==baseline.argumentPacks);
      CHECK(!r.progressionCommandReservations());
    }
  }
  CHECK(!r.stats().bytes&&!r.stats().metalBuffers&&!r.stats().deviceBackings&&!r.stats().hostBackings);
  std::puts("encoding overlap: GPU-first completion, cancellation, deadline, device failure and allocation failure passed");
}
