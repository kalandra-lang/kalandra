#include "Vulkan.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime::vulkan;
namespace {
void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
template<class T> T take(std::expected<T,Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr,"%s: %lld\n",message,static_cast<long long>(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
std::vector<std::byte> pattern(std::size_t size) {
  std::vector<std::byte> result(size);
  for (std::size_t i=0;i<size;++i) result[i]=std::byte((i*37+i/13)&255);
  return result;
}
int run(bool requireStaging) {
  auto driver=take(Runtime::create(),"readback context");
  for (auto size : {0u,1u,3u,65535u,65536u,65537u,1048579u}) {
    const auto input=pattern(size);
    {
      auto buffer=take(driver->uploadBytes(input),"upload bits");
      const auto retained=driver->retainedBytes();
      auto result=take(driver->downloadBytes(buffer),"download exact bits/tails");
      check(result==input && driver->retainedBytes()==retained,"exact bytes and staging retirement");
      std::array<std::thread,4> threads;
      std::atomic<bool> okay{true};
      for (auto &thread:threads) thread=std::thread([&] {
        auto result=driver->downloadBytes(buffer);
        if (!result || *result!=input) okay=false;
      });
      for (auto &thread:threads) thread.join();
      check(okay && driver->retainedBytes()==retained,"concurrent reads retain immutable storage");
    }
    check(!driver->retainedBytes(),"all storage retired");
  }
  const auto input=pattern(1024*1024);
  std::uint64_t sourceBytes;
  {
    auto source=take(driver->uploadBytes(input),"source");
    sourceBytes=driver->retainedBytes();
    auto limited=take(Runtime::create({.maxBytes=sourceBytes}),"bounded readback context");
    auto bounded=take(limited->uploadBytes(input),"exact budget source");
    const auto before=limited->retainedBytes();
    limited->injectFailure(FailurePoint::CommandAllocate);
    check(take(limited->downloadBytes(bounded),"quota uses original mapped read")==input,
          "no additional memory required for a valid transfer");
    limited->injectFailure(FailurePoint::None);
    check(limited->retainedBytes()==before && before<=sourceBytes && !limited->validationErrors(),
          "staging never enlarges byte limit");
  }
  {
    std::vector<float> values(262144);
    for (std::size_t i=0;i<values.size();++i) values[i]=float(i%100)/16;
    auto source=take(driver->upload(values),"device-written input");
    auto sum=take(driver->add(source,source),"GPU writes result");
    const auto before=driver->retainedBytes();
    auto result=take(driver->download(sum),"GPU-written staged download");
    for (std::size_t i=0;i<values.size();++i) check(result[i]==2*values[i],"GPU-written exact result");
    check(take(driver->download(source),"unchanged input")==values &&
          driver->retainedBytes()==before,"GPU copy visibility and immutable input");
  }
  bool staged=true;
  for (auto point : {FailurePoint::CommandAllocate,FailurePoint::BufferCreate,
      FailurePoint::MemoryAllocate,FailurePoint::MemoryBind,FailurePoint::MemoryMap,
      FailurePoint::CommandBegin,FailurePoint::CommandEnd,FailurePoint::FenceCreate,
      FailurePoint::QueueSubmit,FailurePoint::FenceWait}) {
    auto context=take(Runtime::create(),"fault context");
    {
      auto source=take(context->uploadBytes(input),"fault source");
      const auto before=context->retainedBytes();
      std::vector<std::byte> output(input.size(),std::byte{0x52});
      context->injectFailure(point);
      auto result=context->readBytes(source,output);
      if (result && point==FailurePoint::CommandAllocate) {
        staged=false;
        check(!requireStaging,"named GPU must exercise staging");
        std::puts("SKIP: no compatible cached staging memory on this device");
        break;
      }
      const auto expected=point==FailurePoint::FenceWait ? Error::Device : Error::Allocation;
      check(!result && result.error()==expected,"copy failure mapped without suppression");
      check(output==std::vector<std::byte>(input.size(),std::byte{0x52}) &&
            context->retainedBytes()==before,"failed copy leaves host result untouched and retires staging");
      if (point==FailurePoint::QueueSubmit || point==FailurePoint::FenceWait) {
        auto again=context->readBytes(source,output);
        check(!again && again.error()==Error::Device,"uncertain queue remains poisoned");
      } else {
        check(take(context->downloadBytes(source),"copy retry")==input,"recoverable copy retries");
      }
    }
    check(!context->retainedBytes() && !context->validationErrors(),"failed command ownership/validation");
  }
  check(!driver->retainedBytes() && !driver->validationErrors(),"readback corpus resources and validation");
  std::printf("PASS: exact readback/tails/concurrent reads/GPU-written values/byte-budget fallback; staging=%d\n",staged);
  return 0;
}
}
int main(int argc,char **argv) {
  try { return run(argc==2 && std::string_view(argv[1])=="--require-staging"); }
  catch (const std::exception &error) { std::fprintf(stderr,"FAIL: %s\n",error.what());return 1; }
}
