#include "kalandra/CudaArtifact.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
// Private NVRTC-built transform wrapper; ordinary arithmetic uses NVVM.
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  const std::string kind(argv[3]), numeric(argv[4]);
  const bool transpose = kind == "transpose", rms = kind == "rmsNormRows";
  const bool matrix = kind == "matmul64" || kind == "matmul128";
  if (!transpose && !rms && !matrix && kind != "softmaxRows")
    return 2;
  if (!transpose && numeric != "float32")
    return 2;
  if (numeric != "int8" && numeric != "uint8" && numeric != "int16" && numeric != "uint16" &&
      numeric != "int32" && numeric != "uint32" && numeric != "int64" && numeric != "uint64" &&
      numeric != "float32" && numeric != "float64")
    return 2;
  const std::string chip(argv[5]);
  if (chip != "80" && chip != "120")
    return 2;
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 16 * 1024 * 1024)
    return 1;
  kalandra::CudaArtifact artifact;
  artifact.chip = chip == "80" ? 80 : 120;
  artifact.threads = 256;
  artifact.entry = "kalandra_tensor_transform_v1_" + kind + "_" + numeric + "_cuda_desc_v1";
  artifact.buffers =
      transpose ? std::vector<kalandra::CudaBufferSpec>{{8, -1}, {8, -1}, {32, 6}}
      : rms ? std::vector<kalandra::CudaBufferSpec>{{32, -1}, {32, -1}, {32, -1}, {32, -1}, {32, 6}}
            : std::vector<kalandra::CudaBufferSpec>{{32, -1}, {32, -1}, {32, -1}, {32, 6}};
  if (matrix) {
    artifact.tile = kind == "matmul64" ? 64 : 128;
    artifact.threads = 128;
    artifact.entry =
        "kalandra_numeric_v1_matmul_float32_tile" + std::to_string(artifact.tile) + "_cuda_desc_v1";
    artifact.buffers = {{32, -1}, {32, -1}, {32, -1}, {32, 4}};
  }
  input.seekg(0);
  artifact.ptx.assign(std::istreambuf_iterator<char>(input), {});
  if (artifact.ptx.find(".extern .func") != std::string::npos ||
      artifact.ptx.find(artifact.entry) == std::string::npos ||
      artifact.ptx.find("tf32") != std::string::npos ||
      artifact.ptx.find("mma.sync") != std::string::npos)
    return 1;
  auto bytes = kalandra::encodeCudaArtifact(artifact);
  if (!kalandra::decodeCudaArtifact(bytes))
    return 1;
  std::ofstream output(argv[2], std::ios::binary);
  output << bytes;
  output.close();
  return output ? 0 : 1;
}
