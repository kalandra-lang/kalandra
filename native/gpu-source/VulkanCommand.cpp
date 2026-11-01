#include "Supervisor.h"
#include "VulkanBuild.h"
#include "VulkanBuildConfig.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/VulkanSource.h"
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
constexpr auto backendFlag = "vulkan", backendName = "Vulkan";
constexpr bool backendHasASan = KALANDRA_VULKAN_HAS_ASAN, backendOptimizedMatmul = true;
constexpr auto selectedGpuBackend = GpuBackend::Vulkan;
constexpr auto backendNotice = kalandraVulkanNotice;
constexpr auto backendExeSuffix = KALANDRA_VULKAN_EXE_SUFFIX;
constexpr auto backendTransformSuffix = ".vulkan-transform", backendKernelExtension = ".spv";
void buildGpuProgram(const std::filesystem::path &directory,
                     const std::map<std::string, std::string> &outputs, const std::string &variant,
                     ArithmeticMode arithmetic, MatmulPolicy matmul) {
  buildVulkanProgram(directory, outputs, variant, arithmetic, matmul);
}
std::vector<std::string> kernelBuildPaths(const std::string &base, ArithmeticMode arithmetic) {
  std::vector<std::string> paths{base + ".spv.fixed-tree.spv"};
  for (const auto *suffix : {".lowered.mlir", ".converted.mlir", ".kernel.mlir", ".spv"}) {
    paths.push_back(base + suffix);
    if (arithmetic == ArithmeticMode::HardwareWithSoftwareFallback)
      paths.push_back(base + ".spv.software" + suffix);
  }
  return paths;
}
} // namespace
int kalandra::compileVulkanCommand(int argc, char **argv) {
#include "SourceCommand.inc"
}
