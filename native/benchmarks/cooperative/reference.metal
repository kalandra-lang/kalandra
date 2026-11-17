#include <metal_stdlib>
using namespace metal;
struct Words { uint values[1]; };
struct Parameters { uint values[6]; };
// Identical shared-memory stencil, including clamped padding and strict f32.
kernel void kalandra_workgroup_v1_float32_tfloat32r1_g256x(
 device const Words &input [[buffer(0)]], device Words &output [[buffer(1)]],
 device const Parameters &packet [[buffer(2)]], uint lane [[thread_index_in_threadgroup]],
 uint group [[threadgroup_position_in_grid]]) {
 threadgroup float tile[258];
 device const uint *parameters=packet.values;
 ulong n=ulong(parameters[1]) | (ulong(parameters[2])<<32);
 ulong index=ulong(group)*256+lane;
 tile[lane+1]=as_type<float>(input.values[min(index,n-1)]);
 if(lane==0)tile[0]=as_type<float>(input.values[index?min(index-1,n-1):0]);
 if(lane==255)tile[257]=as_type<float>(input.values[min(index+1,n-1)]);
 threadgroup_barrier(mem_flags::mem_threadgroup);
 if(index<n)output.values[index]=as_type<uint>(((tile[lane]+tile[lane+1])+tile[lane+2])/3.0f);
}
