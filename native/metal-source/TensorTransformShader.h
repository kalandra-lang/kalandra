#pragma once
#include "kalandra/NumericType.h"
#include "kalandra/TensorTransform.h"
#include <string>
namespace kalandra {
inline std::string tensorTransformMetal(TensorTransformKind kind, NumericType element) {
  std::string source = R"MSL(#include <metal_stdlib>
using namespace metal;
// Compiler-owned native transform; parameters: count, rows, columns, epsilon
// bits, device-epsilon flag, checked fault. Fast math/FMA are disabled.
)MSL";
  if (kind == TensorTransformKind::Transpose) {
    source += "constant uint kalandra_cell_bytes=" + std::to_string(describeNumeric(element)->bytes()) + ";\n";
    source += R"MSL(kernel void kalandra_tensor_transform_v1(
 device const uchar* input [[buffer(0)]], device const float* epsilon [[buffer(1)]],
 device uchar* output [[buffer(2)]], device float* scratch [[buffer(3)]],
 device uint* p [[buffer(4)]], uint lane [[thread_index_in_threadgroup]],
 uint group [[threadgroup_position_in_grid]]) {
  const ulong i=ulong(group)*256+lane;
  if(i>=p[0]) return;
  const ulong original=(i%p[1])*p[2]+i/p[1];
  for(uint b=0;b<kalandra_cell_bytes;++b)
    output[i*kalandra_cell_bytes+b]=input[original*kalandra_cell_bytes+b];
}
)MSL";
    return source;
  }
  source += "constant bool kalandra_rms=" + std::string(kind == TensorTransformKind::RmsNormRows ? "true" : "false") + ";\n";
  source += R"MSL(kernel void kalandra_tensor_transform_v1(
 device const float* input [[buffer(0)]], device const float* epsilon [[buffer(1)]],
 device float* output [[buffer(2)]], device float* scratch [[buffer(3)]],
 device uint* p [[buffer(4)]], uint lane [[thread_index_in_threadgroup]],
 uint group [[threadgroup_position_in_grid]]) {
  const float eps=kalandra_rms?(p[4]?epsilon[0]:as_type<float>(p[3])):1.0f;
  const uint epsilonBits=as_type<uint>(eps);
  if(kalandra_rms && (!epsilonBits || (epsilonBits&0x80000000u) || (epsilonBits&0x7f800000u)==0x7f800000u)) {
    if(!lane) atomic_fetch_min_explicit((device atomic_uint*)(p+5),4u,memory_order_relaxed);
    return;
  }
  if(!p[0] || group>=p[1]) return;
  const ulong n=p[2], base=ulong(group)*n;
  for(ulong col=lane;col<n;col+=256) {
    const float x=input[base+col];
    scratch[base+col]=kalandra_rms?x*x:x;
  }
  threadgroup_barrier(mem_flags::mem_device);
  // Disjoint aligned subtrees avoid compaction write/read races and preserve
  // odd carries and the existing adjacent-pair reduction order.
  if(!kalandra_rms) {
    for(ulong step=1;step<n;step*=2) {
      for(ulong i=ulong(lane)*2*step;i+step<n;i+=256*2*step) {
        const float a=scratch[base+i], b=scratch[base+i+step];
        scratch[base+i]=(isnan(a)||isnan(b))?NAN:max(a,b);
      }
      threadgroup_barrier(mem_flags::mem_device);
    }
    const float high=scratch[base];
    // Every lane must load the maximum before any lane reuses scratch[base]
    // for exponentials; the preceding reduction barrier alone is insufficient.
    threadgroup_barrier(mem_flags::mem_device);
    if(isnan(high)) {
      for(ulong col=lane;col<n;col+=256) output[base+col]=NAN;
      return;
    }
    const bool positiveInfinity=high==INFINITY;
    for(ulong col=lane;col<n;col+=256) {
      const float x=input[base+col];
      const float value=positiveInfinity?(x==INFINITY?1.0f:0.0f):precise::exp(x-high);
      output[base+col]=value;
      scratch[base+col]=value;
    }
    threadgroup_barrier(mem_flags::mem_device);
  }
  for(ulong step=1;step<n;step*=2) {
    for(ulong i=ulong(lane)*2*step;i+step<n;i+=256*2*step)
      scratch[base+i]=scratch[base+i]+scratch[base+i+step];
    threadgroup_barrier(mem_flags::mem_device);
  }
  const float denominator=kalandra_rms?precise::sqrt((scratch[base]/float(n))+eps):scratch[base];
  for(ulong col=lane;col<n;col+=256)
    output[base+col]=(kalandra_rms?input[base+col]:output[base+col])/denominator;
}
)MSL";
  return source;
}
}
