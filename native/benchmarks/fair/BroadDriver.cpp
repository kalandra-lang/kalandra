#include "../../runtime/Runtime.h"
#include "../../runtime/OrdinaryFault.h"
#include <chrono>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
using I=std::int64_t;
#if defined(ORDINARY_ENTRY)
extern "C" double _mlir_ciface_kalandra_test_entry_ordinary_v1(I,I,kalandra::runtime::OrdinaryFault*);
#elif defined(CHECKED_ENTRY)
extern "C" double _mlir_ciface_kalandra_test_entry(I,I,kalandra::runtime::Invocation*);
#else
extern "C" double broad_entry(I,I);
#endif
int main(int argc,char**argv) {
  if(argc!=6)return 2;
  I seed=std::strtoll(argv[1],nullptr,10),rounds=std::strtoll(argv[2],nullptr,10),
    count=std::strtoll(argv[3],nullptr,10),batches=std::strtoll(argv[4],nullptr,10);
  double expected=std::strtod(argv[5],nullptr);
  if(seed<1 || seed>31 || rounds<0 || rounds>4096 || count<1 || batches<1 || !std::isfinite(expected))return 2;
  for(I batch=-2;batch<batches;++batch) {
    std::uint64_t checksum=0;const auto start=std::chrono::steady_clock::now();
    for(I i=0;i<count;++i) {
#if defined(ORDINARY_ENTRY)
      kalandra::runtime::OrdinaryFault fault;
      auto value=_mlir_ciface_kalandra_test_entry_ordinary_v1(seed,rounds,&fault);
      if(kalandra::runtime::ordinaryFaulted(value,fault))std::abort();
#elif defined(CHECKED_ENTRY)
      kalandra::runtime::Invocation context;
      auto value=_mlir_ciface_kalandra_test_entry(seed,rounds,&context);
      if(context.status() || context.callDepth())std::abort();
      context.settle();if(context.live() || context.allocations()!=context.releases())std::abort();
#else
      auto value=broad_entry(seed,rounds);
#endif
      if(!std::isfinite(value) || std::bit_cast<std::uint64_t>(value)!=std::bit_cast<std::uint64_t>(expected))std::abort();
      checksum+=std::bit_cast<std::uint64_t>(value);
    }
    auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
    std::printf("{\"round\":%lld,\"ns\":%.9g,\"checksum\":%llu}\n",(long long)batch,ns,(unsigned long long)checksum);
  }
}
