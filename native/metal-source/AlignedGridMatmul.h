#pragma once
#include "HardwareShaders.h"
#include <string>
#include <string_view>

// Only whole 16x16 output tiles may use this entry. Keep the original entry
// available for ragged tiles and old bundles; all arithmetic and barriers stay.
inline std::string hardwareAlignedGridMatmulShader() {
  const std::string original = hardwareRegisterMatmulShader;
  const std::string entry = "kernel void kalandra_matmul_hardware_register16(";
  const auto begin = original.find(entry);
  if (begin == std::string::npos)
    return {};
  auto text = original.substr(begin);
  auto replace = [&](std::string_view from, std::string_view to, unsigned expected = 1) {
    std::size_t at = 0;
    unsigned count = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
      text.replace(at, from.size(), to);
      at += to.size();
      ++count;
    }
    return count == expected;
  };
  if (!replace(entry, "kernel void kalandra_matmul_hardware_register16_grid2d_aligned(") ||
      !replace("uint lane [[thread_position_in_threadgroup]]", "uint lane [[thread_index_in_threadgroup]]") ||
      !replace("uint group [[threadgroup_position_in_grid]]", "uint2 group [[threadgroup_position_in_grid]]") ||
      !replace("  const uint tileColumns=n/16+uint(n%16!=0);\n", "") ||
      !replace("const uint rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*16;",
               "const uint rowBase=group.y*16, colBase=group.x*16;") ||
      !replace("r<m ? a.values[r*k+first+at%64] : 0.0f", "a.values[r*k+first+at%64]") ||
      !replace("c<n ? b.values[(first+at/16)*n+c] : 0.0f", "b.values[(first+at/16)*n+c]") ||
      !replace("if(row<m && col<n) {", "{", 2))
    return {};
  return text;
}
