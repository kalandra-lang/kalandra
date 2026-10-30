#pragma once
#include "HardwareShaders.h"
#include <string>

// A permutation of output tiles only. Share the original arithmetic helper and
// keep the old entry byte-for-byte available for other shapes and old bundles.
inline std::string hardwareGroupedMatmulShader() {
  const std::string original = hardwareRegisterMatmulShader;
  const std::string entry = "kernel void kalandra_matmul_hardware_register16(";
  const auto begin = original.find(entry);
  if (begin == std::string::npos)
    return {};
  auto text = original.substr(begin);
  text.replace(0, entry.size(), "kernel void kalandra_matmul_hardware_grouped16(");
  const std::string old =
      "const uint rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*16;";
  const auto at = text.find(old);
  if (at == std::string::npos || text.find(old, at + old.size()) != std::string::npos)
    return {};
  text.replace(at, old.size(), R"MSL(const uint tileRows=(m+15)/16, groupSpan=16u*tileColumns;
  const uint firstRow=(group/groupSpan)*16u;
  const uint height=min(16u,tileRows-firstRow), within=group%groupSpan;
  const uint rowBase=(firstRow+within%height)*16, colBase=(within/height)*16;)MSL");
  return text;
}
