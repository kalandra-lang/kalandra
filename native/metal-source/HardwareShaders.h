#pragma once
// Optional hardware-float32 entries. The offline Metal compiler uses no fast
// math or contraction. Software-policy bundles retain their original entries.
inline constexpr const char *hardwareReductionShader = R"MSL(
struct KalandraHardwareFloat { float values[1]; };
struct KalandraHardwareCount { uint values[1]; };
kernel void kalandra_reduce_sum_hardware_block(
    device KalandraHardwareFloat& input [[buffer(0)]],
    device KalandraHardwareFloat& output [[buffer(1)]],
    device KalandraHardwareCount& parameters [[buffer(2)]],
    uint lane [[thread_position_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]]) {
  threadgroup float values[64];
  const uint n = parameters.values[0];
  const uint base = group * 128;
  const uint left = base + lane * 2;
  if (left < n) {
    float value = input.values[left];
    if (left + 1 < n) value = value + input.values[left + 1];
    values[lane] = value;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  // Each level combines adjacent aligned subtrees. Missing odd leaves carry
  // unchanged, rather than adding padded zeros (which changes signed zero).
  for (uint step = 1; step < 64; step *= 2) {
    const uint index = lane * step * 2;
    if (index < 64 && base + index * 2 < n && base + (index + step) * 2 < n)
      values[index] = values[index] + values[index + step];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (lane == 0) output.values[group] = values[0];
}
)MSL";

inline constexpr const char *hardwareMatmulShader = R"MSL(
struct KalandraHardwareMatrix { float values[1]; };
struct KalandraHardwareShape { uint values[3]; };
kernel void kalandra_matmul_hardware_tiled(
    device KalandraHardwareMatrix& a [[buffer(0)]],
    device KalandraHardwareMatrix& b [[buffer(1)]],
    device KalandraHardwareMatrix& output [[buffer(2)]],
    device KalandraHardwareShape& parameters [[buffer(3)]],
    uint lane [[thread_position_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]]) {
  threadgroup float av[64];
  threadgroup float bv[64];
  const uint m = parameters.values[0], k = parameters.values[1], n = parameters.values[2];
  const uint tileColumns = (n / 8) + uint(n % 8 != 0);
  const uint row = (group / tileColumns) * 8 + lane / 8;
  const uint col = (group % tileColumns) * 8 + lane % 8;
  float stack[29];
  float tail = 0.0f;
  const uint blocks = k / 8;
  for (uint block = 0; block < (k / 8) + uint(k % 8 != 0); ++block) {
    const uint inner = block * 8;
    av[lane] = row < m && inner + lane % 8 < k ? a.values[row * k + inner + lane % 8] : 0.0f;
    bv[lane] = col < n && inner + lane / 8 < k ? b.values[(inner + lane / 8) * n + col] : 0.0f;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (row < m && col < n) {
      float products[8];
      for (uint i = 0; i < 8; ++i)
        products[i] = av[(lane / 8) * 8 + i] * bv[i * 8 + lane % 8];
      uint count = min(8u, k - inner);
      // Explicit adjacent-pair tree; products and sums are separately rounded.
      while (count > 1) {
        for (uint i = 0; i < count / 2; ++i)
          products[i] = products[i * 2] + products[i * 2 + 1];
        if (count & 1) products[count / 2] = products[count - 1];
        count = (count + 1) / 2;
      }
      float value = products[0];
      if (block < blocks) {
        uint occupied = block, level = 0;
        while (occupied & 1) {
          value = stack[level] + value;
          ++level;
          occupied >>= 1;
        }
        stack[level] = value;
      } else tail = value;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (row < m && col < n) {
    float value = tail;
    bool used = (k % 8) != 0;
    for (uint level = 0, occupied = blocks; occupied; ++level, occupied >>= 1) {
      if (occupied & 1) {
        value = used ? stack[level] + value : stack[level];
        used = true;
      }
    }
    output.values[row * n + col] = value;
  }
}

// Full 32-leaf contraction blocks keep the same arithmetic tree while
// amortizing threadgroup synchronization across four old eight-leaf blocks.
inline float kalandra_hardware_eight(threadgroup float* a, threadgroup float* b,
                                      uint row, uint col, uint first) {
  float p0=a[row*32+first+0]*b[(first+0)*8+col];
  float p1=a[row*32+first+1]*b[(first+1)*8+col];
  float p2=a[row*32+first+2]*b[(first+2)*8+col];
  float p3=a[row*32+first+3]*b[(first+3)*8+col];
  float p4=a[row*32+first+4]*b[(first+4)*8+col];
  float p5=a[row*32+first+5]*b[(first+5)*8+col];
  float p6=a[row*32+first+6]*b[(first+6)*8+col];
  float p7=a[row*32+first+7]*b[(first+7)*8+col];
  return ((p0+p1)+(p2+p3))+((p4+p5)+(p6+p7));
}
kernel void kalandra_matmul_hardware_block32(
    device KalandraHardwareMatrix& a [[buffer(0)]],
    device KalandraHardwareMatrix& b [[buffer(1)]],
    device KalandraHardwareMatrix& output [[buffer(2)]],
    device KalandraHardwareShape& parameters [[buffer(3)]],
    uint lane [[thread_position_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]]) {
  threadgroup float av[256];
  threadgroup float bv[256];
  const uint m=parameters.values[0], k=parameters.values[1], n=parameters.values[2];
  const uint tileColumns=n/8+uint(n%8!=0);
  const uint rowBase=(group/tileColumns)*8, colBase=(group%tileColumns)*8;
  const uint row=rowBase+lane/8, col=colBase+lane%8;
  float stack[27];
  const uint blocks=k/32; // Runtime admits only nonzero multiples of 32.
  for (uint block=0; block<blocks; ++block) {
    const uint first=block*32;
    for (uint at=lane; at<256; at+=64) {
      const uint r=rowBase+at/32, c=colBase+at%8;
      av[at]=r<m ? a.values[r*k+first+at%32] : 0.0f;
      bv[at]=c<n ? b.values[(first+at/8)*n+c] : 0.0f;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (row<m && col<n) {
      float r0=kalandra_hardware_eight(av,bv,lane/8,lane%8,0);
      float r1=kalandra_hardware_eight(av,bv,lane/8,lane%8,8);
      float r2=kalandra_hardware_eight(av,bv,lane/8,lane%8,16);
      float r3=kalandra_hardware_eight(av,bv,lane/8,lane%8,24);
      float value=(r0+r1)+(r2+r3);
      uint occupied=block, level=0;
      while (occupied&1) {
        value=stack[level]+value;
        ++level; occupied>>=1;
      }
      stack[level]=value;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (row<m && col<n) {
    bool used=false;
    float value=0.0f;
    for (uint level=0, occupied=blocks; occupied; ++level, occupied>>=1) {
      if (occupied&1) {
        value=used ? stack[level]+value : stack[level];
        used=true;
      }
    }
    output.values[row*n+col]=value;
  }
}

// A 16x8 output tile reuses each RHS panel across sixteen rows. Its 128
// lanes reduce exact 64-leaf subtrees with two barriers per block. Missing
// output lanes participate in both barriers but never enter the arithmetic.
inline float kalandra_hardware_eight64(threadgroup float* a, threadgroup float* b,
                                      uint row, uint col, uint first) {
  float p0=a[row*64+first+0]*b[(first+0)*8+col];
  float p1=a[row*64+first+1]*b[(first+1)*8+col];
  float p2=a[row*64+first+2]*b[(first+2)*8+col];
  float p3=a[row*64+first+3]*b[(first+3)*8+col];
  float p4=a[row*64+first+4]*b[(first+4)*8+col];
  float p5=a[row*64+first+5]*b[(first+5)*8+col];
  float p6=a[row*64+first+6]*b[(first+6)*8+col];
  float p7=a[row*64+first+7]*b[(first+7)*8+col];
  return ((p0+p1)+(p2+p3))+((p4+p5)+(p6+p7));
}
kernel void kalandra_matmul_hardware_block64(
    device KalandraHardwareMatrix& a [[buffer(0)]],
    device KalandraHardwareMatrix& b [[buffer(1)]],
    device KalandraHardwareMatrix& output [[buffer(2)]],
    device KalandraHardwareShape& parameters [[buffer(3)]],
    uint lane [[thread_position_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]]) {
  threadgroup float av[1024];
  threadgroup float bv[512];
  const uint m=parameters.values[0], k=parameters.values[1], n=parameters.values[2];
  const uint tileColumns=n/8+uint(n%8!=0);
  const uint rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*8;
  const uint row=rowBase+lane/8, col=colBase+lane%8;
  float stack[27];
  const uint blocks=k/64; // Runtime admits only nonzero multiples of 64.
  for (uint block=0; block<blocks; ++block) {
    const uint first=block*64;
    for (uint at=lane; at<1024; at+=128) {
      const uint r=rowBase+at/64;
      av[at]=r<m ? a.values[r*k+first+at%64] : 0.0f;
    }
    for (uint at=lane; at<512; at+=128) {
      const uint c=colBase+at%8;
      bv[at]=c<n ? b.values[(first+at/8)*n+c] : 0.0f;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (row<m && col<n) {
      float r0=kalandra_hardware_eight64(av,bv,lane/8,lane%8,0);
      float r1=kalandra_hardware_eight64(av,bv,lane/8,lane%8,8);
      float r2=kalandra_hardware_eight64(av,bv,lane/8,lane%8,16);
      float r3=kalandra_hardware_eight64(av,bv,lane/8,lane%8,24);
      float r4=kalandra_hardware_eight64(av,bv,lane/8,lane%8,32);
      float r5=kalandra_hardware_eight64(av,bv,lane/8,lane%8,40);
      float r6=kalandra_hardware_eight64(av,bv,lane/8,lane%8,48);
      float r7=kalandra_hardware_eight64(av,bv,lane/8,lane%8,56);
      float value=((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7));
      uint occupied=block, level=0;
      while (occupied&1) {
        value=stack[level]+value;
        ++level; occupied>>=1;
      }
      stack[level]=value;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (row<m && col<n) {
    bool used=false;
    float value=0.0f;
    for (uint level=0, occupied=blocks; occupied; ++level, occupied>>=1) {
      if (occupied&1) {
        value=used ? stack[level]+value : stack[level];
        used=true;
      }
    }
    output.values[row*n+col]=value;
  }
}
)MSL";
