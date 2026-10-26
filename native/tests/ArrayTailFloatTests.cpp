#include "../runtime/NativeCancellation.h"
#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <bit>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
#ifdef FLOAT32
using Float = float;
using Bits = std::uint32_t;
constexpr Bits sign = 0x80000000u;
constexpr std::array<Bits,10> patterns{0,sign,1,sign|1,0x7f800000u,0xff800000u,
                                      0x7fc12345u,0x7f812345u,0x3f800000u,0x7f7fffffu};
#else
using Float = double;
using Bits = std::uint64_t;
constexpr Bits sign = 0x8000000000000000ULL;
constexpr std::array<Bits,10> patterns{0,sign,1,sign|1,0x7ff0000000000000ULL,
    0xfff0000000000000ULL,0x7ff8123456789abcULL,0x7ff0123456789abcULL,
    0x3ff0000000000000ULL,0x7fefffffffffffffULL};
#endif
struct Descriptor { void *allocated; Float *data; std::int64_t offset,size,stride; };
extern "C" Float _mlir_ciface_kalandra_test_entry(const Descriptor *,std::int64_t,Invocation *);
int main() {
  const bool interrupts = initializeNativeCancellation();
  for (int mode = 0; mode != 4; ++mode)
    for (Bits bits : patterns)
      for (std::int64_t rounds : {0,1,16,1025}) {
        Invocation context;
        context.failAllocationAt(0);
        std::atomic<bool> token{false};
        std::atomic<std::uint64_t> polls{0};
        if (mode) context.bindCancellation(&token);
        if (mode == 2) context.watchPolls(&polls);
        if (mode == 3) context.allowNativeInterrupts(interrupts);
        Float input = std::bit_cast<Float>(bits);
        const Descriptor descriptor{&input,&input,0,1,1};
        const auto result = _mlir_ciface_kalandra_test_entry(&descriptor,rounds,&context);
        if (std::bit_cast<Bits>(result) != (bits ^ (rounds%2 ? sign : 0)) ||
            std::bit_cast<Bits>(input) != bits || context.status() || context.callDepth())
          throw std::runtime_error("array loop changed an IEEE bit pattern or borrowed input");
        context.settle();
        if (context.live() || context.allocations() != context.releases())
          throw std::runtime_error("floating array loop leaked backing");
      }
  std::cout << "Array float tail: 160 exact bit-pattern/mode cases preserve zero signs, "
               "subnormals, infinities, NaN payloads and immutable borrowed inputs\n";
}
