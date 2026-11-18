typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
// The compiler's memref1d descriptor ABI has five arguments per buffer.
extern "C" __global__ __launch_bounds__(256) void
kalandra_workgroup_v1_float32_tfloat32r1_g256x_cuda_desc_v1(
 const float*, const float* input, int64_t, int64_t, int64_t,
 float*, float* output, int64_t, int64_t, int64_t,
 uint32_t*, uint32_t* parameters, int64_t, int64_t, int64_t) {
 __shared__ float tile[258];
 uint64_t n=uint64_t(parameters[1]) | (uint64_t(parameters[2])<<32);
 uint64_t index=uint64_t(blockIdx.x)*256+threadIdx.x;
 unsigned lane=threadIdx.x;
 tile[lane+1]=input[index<n?index:n-1];
 if(lane==0)tile[0]=input[index?(index-1<n?index-1:n-1):0];
 if(lane==255)tile[257]=input[index+1<n?index+1:n-1];
 __syncthreads();
 if(index<n)output[index]=((tile[lane]+tile[lane+1])+tile[lane+2])/3.0f;
}
