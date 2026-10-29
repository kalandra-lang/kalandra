#include "../runtime/Tensor.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef GPU
#include "../metal/Metal.h"
#endif
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto Element = NumericType::ELEMENT;
using Cell = NumericValue<Element>;
using Matrix = CpuTensor<Element, 2>;
#ifndef GPU
#if OP == 2
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Cell, Invocation *);
#else
extern "C" void _mlir_ciface_kalandra_test_entry(Matrix *, Matrix *, Invocation *);
#endif
#endif
static void check(bool value) {
  if (!value) { std::fputs("Tensor transform ownership/ABI check failed\n", stderr); std::abort(); }
}
#ifdef GPU
namespace m = kalandra::runtime::metal;
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation ctx;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  check(core.observe(task, m::Runtime::bufferLayout(), ctx, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value; value->~Buffer(); return result;
}
static void retire(TaskCore &core, GroupHandle group) {
  check(core.close(group) && core.drain() && core.release(group));
  const auto s=core.stats();
  check(!s.groups && !s.records && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
#endif
static int oneCase(int argc, char **argv) {
  check(argc == 9 || argc == 10);
  const std::int64_t rows=std::stoll(argv[1]), cols=std::stoll(argv[2]);
  const int layout=std::stoi(argv[3]), cancel=std::stoi(argv[6]);
  const auto epsilonBits=std::stoull(argv[4]), fail=std::stoull(argv[5]);
  check(rows>=0 && cols>=0 && rows*cols<=4000000);
  std::vector<Cell> flat(rows*cols);
  std::ifstream stream(argv[7],std::ios::binary);
  stream.read(reinterpret_cast<char *>(flat.data()),flat.size()*sizeof(Cell));
  check(std::size_t(stream.gcount())==flat.size()*sizeof(Cell));
  Cell epsilon{}; std::memcpy(&epsilon,&epsilonBits,sizeof(Cell));
  std::vector<Cell> output;
  std::int64_t status=0; bool cancelled=false;
  std::uint64_t allocations=0;
#ifndef GPU
  const auto pitch=cols*2+3;
  std::vector<Cell> backing(rows*pitch+2*cols+16);
  const std::int64_t rowStride=(layout&1)?-pitch:pitch, colStride=(layout&2)?-2:2;
  const auto offset=5+((layout&1)&&rows?(rows-1)*pitch:0)+((layout&2)&&cols?(cols-1)*2:0);
  Matrix input{backing.data(),backing.data(),offset,{rows,cols},{rowStride,colStride}}, result{};
  for(std::int64_t r=0;r<rows;++r) for(std::int64_t c=0;c<cols;++c)
    std::memcpy(&backing[offset+r*rowStride+c*colStride],&flat[r*cols+c],sizeof(Cell));
  const auto original=backing;
  Invocation context;
  context.failAllocationAt(fail);
  std::atomic<bool> token{cancel==1},done{false},ready{false};
  std::atomic<std::uint64_t> polls{0};
  std::thread observer;
  if(cancel) context.bindCancellation(&token);
  if(cancel==2) {
    context.watchPolls(&polls);
    observer=std::thread([&] { ready=true; while(polls.load()<32 && !done.load()) std::this_thread::yield(); if(!done.load()) token=true; });
    while(!ready.load()) std::this_thread::yield();
  }
#if OP == 2
  _mlir_ciface_kalandra_test_entry(&result,&input,epsilon,&context);
#else
  _mlir_ciface_kalandra_test_entry(&result,&input,&context);
#endif
  done=true; if(observer.joinable()) observer.join();
  check(!context.callDepth() && std::memcmp(original.data(),backing.data(),backing.size()*sizeof(Cell))==0);
  status=context.status();cancelled=context.poll()==-1;
  if(!status && !cancelled) {
    const auto m=OP==0?cols:rows,n=OP==0?rows:cols;
    check(result.sizes[0]==m && result.sizes[1]==n);
    output.resize(m*n);
    for(std::int64_t r=0;r<m;++r) for(std::int64_t c=0;c<n;++c)
      std::memcpy(&output[r*n+c],result.aligned+result.offset+r*result.strides[0]+c*result.strides[1],sizeof(Cell));
  }
  allocations=context.allocations();context.settle();
  check(!context.live() && context.allocations()==context.releases());
#else
  check(argc==10);
  m::Options options;
  // Held-completion cases deliberately exercise the physical GPU path. A
  // contiguous transpose view has no device completion to hold or fail.
  options.contiguousTranspose=cancel<2;
  auto made=m::Runtime::create(options);check(bool(made));auto &runtime=**made;
  auto device=runtime.device();check(bool(device));
  const auto kind=OP==0?TensorTransformKind::Transpose:OP==1?TensorTransformKind::SoftmaxRows:TensorTransformKind::RmsNormRows;
  auto kernel=runtime.loadTensorTransformKernel(std::string(argv[9])+"/kernel0.metallib",*device,kind,Element,OP==2 && layout==1);
  check(bool(kernel));
  auto core=TaskCore::create();check(bool(core));
  {
    auto host=runtime.hostTensor(std::as_bytes(std::span(flat)),std::array{rows,cols},*m::storageFormat(Element));check(bool(host));
    auto upload=[&](m::Buffer value) {
      auto group=core->openRoot();auto task=runtime.upload(*core,group,value,*device);
      check(task && core->drain());auto result=observe(*core,task);retire(*core,group);return result;
    };
    auto input=upload(*host);
    m::Buffer deviceEpsilon;
    if(OP==2 && layout==1) {
      auto h=runtime.hostTensor(std::as_bytes(std::span(&epsilon,1)),std::array<std::int64_t,0>{},*m::storageFormat(Element));
      check(bool(h));deviceEpsilon=upload(*h);
    }
    const auto baseline=runtime.stats().bytes;
    runtime.failAllocationAt(fail);
    auto group=core->openRoot();
    if(cancel==1) check(core->cancel(group));
    if(cancel>=2) runtime.holdCompletions(true);
    std::array<std::uint32_t,2> operands{0,1};std::array<std::uint64_t,1> scalars{epsilonBits};
    const auto ins=std::span(operands).first(deviceEpsilon?2:1);
    const auto captures=std::span(scalars).first(OP==2 && !deviceEpsilon?1:0);
    const auto destination=deviceEpsilon?2u:1u;
    const std::array<m::NumericInstruction,1> code{{{m::NumericInstructionKind::Map,*kernel,ins,captures,destination,0}}};
    const std::array<m::Buffer,2> inputs{input,deviceEpsilon};
    auto task=runtime.numericProgram(*core,group,std::span(inputs).first(deviceEpsilon?2:1),code,destination,Element,false);
    check(bool(task));
    if(cancel>=2) {
      auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
      while(!core->stats().externalPending) { check(std::chrono::steady_clock::now()<end);core->step();std::this_thread::yield(); }
      if(cancel==4) runtime.failNextCompletion();else check(core->cancel(group,cancel==3));
      check(runtime.stats().bytes>baseline);
      runtime.holdCompletions(false);
    }
    check(core->drain());
    const auto failure=core->failure(task);
    if(cancel>=1 && cancel<=3) check(failure.kind==TaskFailure::Cancelled);
    if(cancel==3) check(core->failure(group).kind==TaskFailure::DeadlineExceeded);
    if(cancel==4) check(failure.kind==TaskFailure::Fault && failure.code==std::int64_t(m::Error::Device));
    status=failure.code;cancelled=failure.kind==TaskFailure::Cancelled;
    allocations=runtime.stats().attempts;
    runtime.failAllocationAt(0);
    if(failure.kind==TaskFailure::None) {
      auto result=observe(*core,task);
      auto download=runtime.download(*core,group,result);check(download && core->drain());
      auto h=observe(*core,download);auto bytes=h.hostBytes();check(bool(bytes));
      output.resize(rows*cols);check(bytes->size()==output.size()*sizeof(Cell));
      std::memcpy(output.data(),bytes->data(),bytes->size());
      check(h.shape().size()==2 && h.shape()[0]==(OP==0?cols:rows) && h.shape()[1]==(OP==0?rows:cols));
    }
    retire(*core,group);
    check(runtime.stats().bytes==baseline && !runtime.stats().inFlight && !runtime.stats().argumentPacks);
    // Verify the original device input also survived without mutation.
    auto g=core->openRoot();auto d=runtime.download(*core,g,input);check(d && core->drain());
    auto unchanged=observe(*core,d);auto bytes=unchanged.hostBytes();check(bool(bytes));
    check(bytes->size()==flat.size()*sizeof(Cell) && std::memcmp(bytes->data(),flat.data(),bytes->size())==0);
    unchanged={};retire(*core,g);
  }
  auto s=runtime.stats();check(!s.bytes && !s.metalBuffers && !s.stagingBuffers && !s.argumentPacks && !s.inFlight);
#endif
  std::printf("{\"status\":%lld,\"cancelled\":%s,\"allocations\":%llu,\"bits\":[",(long long)status,cancelled?"true":"false",(unsigned long long)allocations);
  for(std::size_t i=0;i<output.size();++i) {std::uint64_t bits=0;std::memcpy(&bits,&output[i],sizeof(Cell));std::printf("%s%llu",i?",":"",(unsigned long long)bits);}
  std::puts("]}");
  std::fflush(stdout);
  return 0;
}
int main(int argc,char**argv) {
  if(argc==2 && std::string_view(argv[1])=="--server") {
    // Amortize sanitizer/Metal process initialization, never test computation.
    // Each request still creates and completely destroys its own invocation,
    // task core, runtime, input allocations, pipeline, and result owners.
    std::string line;
    while(std::getline(std::cin,line)) {
      std::istringstream input(line);std::vector<std::string> words{argv[0]};std::string word;
      while(input>>std::quoted(word)) words.push_back(word);
      std::vector<char*> args;for(auto&value:words) args.push_back(value.data());
      oneCase(int(args.size()),args.data());
    }
    return 0;
  }
  return oneCase(argc,argv);
}
