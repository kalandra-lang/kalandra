#pragma once
#include "../third_party/mlx-steel/SteelUpstream.h"
#include <string>

// Retain the existing 12-byte shape and reflected buffer ABI. The upstream
// loaders, GEMM loop, SIMD matrix arithmetic and safe stores are unmodified.
// Only entry binding, shape derivation and the already validated post-map are ours.
inline std::string steelMatmulShader(bool fused = false, const std::string &extra = {},
                                      const std::string &arguments = {}) {
  std::string text = "\n#ifndef KALANDRA_STEEL_UPSTREAM\n#define KALANDRA_STEEL_UPSTREAM\n";
  text += "\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wunused-const-variable\"\n";
  text += "#pragma clang diagnostic ignored \"-Wc++17-extensions\"\n";
  text += kalandraSteelUpstream;
  text += "\n#pragma clang diagnostic pop\n";
  text += "\n#endif\n";
  for (unsigned tile = 0; tile < 2; ++tile)
    for (unsigned mn = 0; mn < 2; ++mn)
      for (unsigned kk = 0; kk < 2; ++kk) {
        const auto name = std::string("kalandra_matmul_steel_") + (fused ? "fused_" : "") +
                          (tile ? "64x32_" : "64_") + std::to_string(mn) + "_" +
                          std::to_string(kk);
        text += "\nkernel void " + name + R"MSL((
 device KalandraHardwareMatrix& a [[buffer(0)]],
 device KalandraHardwareMatrix& b [[buffer(1)]],
 device KalandraHardwareMatrix& output [[buffer(2)]],
 device KalandraHardwareShape& shape [[buffer(3)]],
)MSL" + extra + R"MSL(
 uint lane [[thread_index_in_simdgroup]],
 uint sg [[simdgroup_index_in_threadgroup]],
 uint2 group [[threadgroup_position_in_grid]]) {
)MSL";
        text += "using K=mlx::steel::GEMMKernel<float,float,64," +
                std::string(tile ? "32,32" : "64,16") + ",2,2,false,false," +
                (mn ? "true" : "false") + "," + (kk ? "true" : "false") + ",float>;\n";
        text += R"MSL(
 using MMA=typename K::mma_t;
 threadgroup float aa[K::tgp_mem_size_a], bb[K::tgp_mem_size_b];
 const uint m=shape.values[0], k=shape.values[1], n=shape.values[2];
 const uint rowBase=group.y*64, colBase=group.x*)MSL" + std::string(tile ? "32" : "64") + R"MSL(;
 const short rows=short(min(64u,m-rowBase));
 const short columns=short(min()MSL" + std::string(tile ? "32u" : "64u") + R"MSL(,n-colBase));
 thread typename K::loader_a_t loadA(a.values+size_t(rowBase)*k,int(k),aa,sg,lane);
 thread typename K::loader_b_t loadB(b.values+colBase,int(n),bb,sg,lane);
 thread MMA mma(sg,lane);
)MSL";
        const std::string bk = tile ? "32" : "16";
        text += "K::gemm_loop<" + std::string(mn ? "true,true," : "false,false,") +
                (kk ? "true" : "false") + ">(aa,bb,int(k/" + bk +
                "),loadA,loadB,mma,rows,columns,short(k%" + bk + "));\n";
        if (fused)
          text += R"MSL(
 STEEL_PRAGMA_UNROLL
 for(short i=0;i<MMA::TM;++i) {
  STEEL_PRAGMA_UNROLL
  for(short j=0;j<MMA::TN;++j) {
   thread auto& cellValues=mma.Ctile.frag_at(i,j);
   STEEL_PRAGMA_UNROLL
   for(short at=0;at<decltype(mma.Ctile)::kElemsPerFrag;++at) {
    const uint row=rowBase+mma.sm+i*MMA::TM_stride;
    const uint col=colBase+mma.sn+j*MMA::TN_stride+at;
    if(row<m && col<n)
     cellValues[at]=kalandra_fused_post(cellValues[at])MSL" + arguments + R"MSL(,row*n+col);
   }
  }
 }
)MSL";
        text += "device float* target=output.values+size_t(rowBase)*n+colBase;\n";
        text += mn ? "mma.store_result(target,int(n));\n" :
                     "mma.store_result_safe(target,int(n),short2(columns,rows));\n";
        text += "}\n";
      }
  return text;
}
