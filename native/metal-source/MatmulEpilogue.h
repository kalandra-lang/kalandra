#pragma once
#include "GroupedMatmul.h"
#include "HardwareShaders.h"
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// Lift only the pinned SPIRV-Cross canonical straight-line, single-cell map.
// This is a code-generation optimization over validated compiler output, not a
// general MSL parser. Any unsupported spelling/control/indexing keeps the map.
inline std::optional<std::string> matmulEpilogue(const std::string &source, unsigned tensors,
                                                 unsigned scalars) {
  if (!tensors || tensors > 27 || scalars == UINT32_MAX || source.size() > 16384)
    return {};
  const std::string name = "kalandra_elementwise_v2";
  const auto start = source.find("kernel void " + name + "(");
  if (start == std::string::npos)
    return {};
  const auto opening = source.find('{', start);
  const auto end = source.find_last_of('}');
  if (opening == std::string::npos || end == std::string::npos || opening >= end)
    return {};
  const auto signature = source.substr(start, opening - start);
  std::regex argument(
      R"(device\s+(\w+)&\s+(kalandra_elementwise_v2_arg_(\d+))\s*\[\[buffer\((\d+)\)\]\])");
  std::vector<std::string> types, names;
  for (auto it = std::sregex_iterator(signature.begin(), signature.end(), argument);
       it != std::sregex_iterator(); ++it) {
    const auto &match = *it;
    if (match[3].str() != std::to_string(names.size()) ||
        match[4].str() != std::to_string(names.size()))
      return {};
    types.push_back(match[1]);
    names.push_back(match[2]);
  }
  if (names.size() != tensors + 2)
    return {};
  for (unsigned i = 0; i <= tensors; ++i)
    if (!std::regex_search(source,
                           std::regex("struct " + types[i] + R"(\s*\{\s*float _m0\[1\];\s*\};)")))
      return {};
  if (!std::regex_search(source,
                         std::regex("struct " + types.back() + R"(\s*\{\s*uint _m0\[)" +
                                    std::to_string(std::uint64_t(scalars) + 1) + R"(\];\s*\};)")))
    return {};
  const auto parameter = names.back();
  const auto body = source.substr(opening + 1, end - opening - 1);
  std::smatch match;
  std::regex canonical("\\s*uint (\\w+) = \\(gl_WorkGroupID\\.x \\* 64u\\) \\+ "
                       "gl_LocalInvocationID\\.x;\\s*if \\(\\1 < " +
                       parameter + R"(\._m0\[0u\]\)\s*\{([^{}]*)\}\s*)");
  if (!std::regex_match(body, match, canonical))
    return {};
  const std::string index = match[1];
  std::string code = match[2];
  const auto replace = [](std::string &text, const std::string &from, const std::string &to) {
    std::size_t at = 0, count = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
      text.replace(at, from.size(), to);
      at += to.size();
      ++count;
    }
    return count;
  };
  for (unsigned i = 0; i < tensors; ++i) {
    auto probe = code;
    const auto load = names[i] + "._m0[" + index + "]";
    if (!replace(probe, load, "kalandra_value") ||
        std::regex_search(probe, std::regex("\\b" + names[i] + "\\b")))
      return {};
    if (i == 0)
      replace(code, load, "kalandra_matrix_value");
  }
  const auto output = names[tensors];
  const std::regex store(output + "\\._m0\\[" + index + R"(\] = ([^;]+);\s*$)");
  if (!std::regex_search(code, match, store))
    return {};
  const auto expression = match[1].str();
  code.replace(match.position(), match.length(), "return " + expression + ";\n");
  if (code.find(output) != std::string::npos || code.find("gl_") != std::string::npos)
    return {};
  std::string helper = "\ninline float kalandra_fused_post(float kalandra_matrix_value";
  std::string arguments;
  std::string extra;
  for (unsigned i = 1; i < tensors; ++i) {
    helper += ",device " + types[i] + "& " + names[i];
    arguments += "," + names[i];
    extra += "    device " + types[i] + "& " + names[i] + " [[buffer(" + std::to_string(i + 3) +
             ")]],\n";
  }
  helper += ",device " + types.back() + "& " + parameter + ",uint " + index + ") {\n";
  // Retain the parameter binding even for maps without scalar captures.
  helper += "if(" + index + ">=" + parameter + "._m0[0u]) return kalandra_matrix_value;\n";
  helper += code + "}\n";
  arguments += "," + parameter;
  extra += "    device " + types.back() + "& " + parameter + " [[buffer(" +
           std::to_string(tensors + 3) + ")]],\n";
  std::string matrices = std::string(hardwareMatmulShader) + hardwareRegisterMatmulShader +
                         hardwareGroupedMatmulShader();
  replace(matrices, "kalandra_matmul_hardware_", "kalandra_matmul_fused_");
  if (replace(matrices, "    uint lane [[thread_position_in_threadgroup]],",
              extra + "    uint lane [[thread_position_in_threadgroup]],") != 5)
    return {};
  unsigned stores = 0;
  for (auto cell : {std::string("row*n+col"), std::string("row * n + col")}) {
    const auto storeText = "output.values[" + cell + "]";
    stores += replace(matrices, storeText + (cell == "row*n+col" ? "=value;" : " = value;"),
                      storeText + "=kalandra_fused_post(value" + arguments + "," + cell + ");");
  }
  if (stores != 5)
    return {};
  return helper + matrices;
}
