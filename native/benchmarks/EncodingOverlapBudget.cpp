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


int main(int argc,char **argv) {
 CHECK(argc==2);std::vector<std::uint32_t> expected;
 for(std::uint64_t budget=15728640;budget<=15794176;budget+=4096) {
  m::Options options;options.maxBytes=budget;auto made=m::Runtime::create(options);CHECK(made);auto &r=**made;
  {
   Program program(r,argv[1]);transformer::Inputs data(64,512,1);std::array<m::Buffer,7> input;
   for(unsigned i: {5u,6u,1u,2u,3u,4u,0u}) {
    auto c=TaskCore::create();auto g=c->openRoot();
    auto h=r.hostTensor(std::as_bytes(std::span(data.values[i])),data.shapes[i],m::Format::Float32);CHECK(h);
    auto t=r.upload(*c,g,*h,*r.device());CHECK(t&&c->drain()&&c->failure(t).kind==TaskFailure::None);
    input[i]=observe(*c,t);retire(*c,g);
   }
   auto c=TaskCore::create();auto g=c->openRoot();const auto before=r.stats();r.holdCompletions(true);
   auto t=program.launch(r,*c,g,input,transformer::epsilon,data.scale);CHECK(t);
   while(!c->stats().externalPending&&!c->settled(t))c->step();
   auto held=r.stats();auto credits=r.progressionCommandReservations();r.holdCompletions(false);CHECK(c->drain());
   auto failure=c->failure(t);auto commands=r.stats().submissions-before.submissions;
   if(failure.kind==TaskFailure::None) {
    auto actual=download(r,*c,g,observe(*c,t));if(expected.empty())expected=actual;CHECK(actual==expected);
   }
   std::printf("%llu credits=%llu params=%llu commands=%llu peak=%llu failure=%u/%lld\n",(unsigned long long)budget,(unsigned long long)credits,(unsigned long long)held.stagingBuffers,(unsigned long long)commands,(unsigned long long)r.stats().peakBytes,unsigned(failure.kind),(long long)failure.code);
   retire(*c,g);CHECK(r.stats().peakBytes<=budget);
  }
  CHECK(!r.stats().bytes&&!r.stats().metalBuffers&&!r.stats().inFlight);
 }
}
