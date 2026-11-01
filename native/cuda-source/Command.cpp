#include "../gpu-source/Supervisor.h"
#include "Build.h"
#include "CudaBuildConfig.h"
#include "kalandra/CudaSource.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using namespace kalandra;
namespace {
constexpr auto backendFlag = "cuda", backendName = "CUDA";
constexpr bool backendHasASan = false, backendOptimizedMatmul = true;
constexpr auto selectedGpuBackend = GpuBackend::Cuda;
constexpr auto backendNotice = kalandraCudaNotice;
constexpr auto backendExeSuffix = "";
constexpr auto backendTransformSuffix = ".cuda-transform", backendKernelExtension = ".ptx";
void buildGpuProgram(const std::filesystem::path &directory,
                     const std::map<std::string, std::string> &outputs, const std::string &variant,
                     ArithmeticMode arithmetic, MatmulPolicy matmul) {
  buildCudaProgram(directory, outputs, variant, arithmetic, matmul);
}
std::vector<std::string> kernelBuildPaths(const std::string &base, ArithmeticMode arithmetic) {
  std::vector<std::string> paths{base + ".ptx", base + ".ptx.json", base + ".ptx.fixed-tree.ptx",
                                 base + ".ptx.fixed-tree.ptx.json"};
  if (arithmetic == ArithmeticMode::HardwareWithSoftwareFallback) {
    paths.push_back(base + ".ptx.software.ptx");
    paths.push_back(base + ".ptx.software.ptx.json");
  }
  return paths;
}
} // namespace
int kalandra::compileCudaCommand(int argc, char **argv) {
#include "../gpu-source/SourceCommand.inc"
}
