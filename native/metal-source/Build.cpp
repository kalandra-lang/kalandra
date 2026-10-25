#include "Build.h"
#include "HardwareShaders.h"
#include "Toolchain.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
void run(std::vector<std::string> command) {
  std::vector<llvm::StringRef> args(command.begin(), command.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(command[0], args, {}, {}, 0, 0, &error) != 0)
    throw std::runtime_error("GPU build stage failed: " + command[0] + " " + error);
}
#ifdef KALANDRA_INSTALLED_COMPILER
std::string capture(std::vector<std::string> command) {
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  if (auto error =
          llvm::sys::fs::createTemporaryFile("kalandra-platform", "txt", descriptor, temporary))
    throw std::runtime_error(error.message());
  llvm::sys::fs::closeFile(descriptor);
  llvm::FileRemover cleanup(temporary);
  std::array<std::optional<llvm::StringRef>, 3> redirects{std::nullopt, llvm::StringRef(temporary),
                                                          std::nullopt};
  std::vector<llvm::StringRef> args(command.begin(), command.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(command[0], args, {}, redirects, 0, 0, &error) != 0)
    throw std::runtime_error("Apple platform tool failed: " + command[0] + " " + error);
  std::ifstream stream{std::string(temporary)};
  std::ostringstream output;
  output << stream.rdbuf();
  auto result = output.str();
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
}
#endif
#ifdef KALANDRA_INSTALLED_COMPILER
std::filesystem::path resourceRoot() {
  static int anchor;
  std::error_code error;
  auto executable =
      std::filesystem::canonical(llvm::sys::fs::getMainExecutable(nullptr, &anchor), error);
  if (error)
    throw std::runtime_error("cannot locate installed Metal compiler executable");
  return executable.parent_path() / KALANDRA_RUNTIME_RELATIVE_DIR;
}
#endif
std::string tool(const char *configured, const char *name) {
#ifdef KALANDRA_INSTALLED_COMPILER
  auto directory = (resourceRoot() / "../../../libexec/kalandra").lexically_normal();
  if (std::filesystem::exists(resourceRoot().parent_path() / "toolchain") ||
      std::filesystem::exists(directory)) {
    auto path = directory / name;
    if (!std::filesystem::is_regular_file(path))
      throw std::runtime_error("bundled Metal tool is missing: " + path.string());
    return path.string();
  }
#else
  (void)name;
#endif
  return configured;
}
std::string platformMetal() {
#ifdef KALANDRA_INSTALLED_COMPILER
  const char *explicitCompiler = std::getenv("KALANDRA_METAL_COMPILER");
  auto compiler = explicitCompiler && *explicitCompiler
                      ? std::string(explicitCompiler)
                      : capture({"/usr/bin/xcrun", "--find", "metal"});
  if (!std::filesystem::path(compiler).is_absolute() || !std::filesystem::is_regular_file(compiler))
    throw std::runtime_error(
        "Apple Metal compiler is unavailable; install the pinned Xcode Metal Toolchain");
  run({"/usr/bin/codesign", "--verify", "--strict", "-R=anchor apple", compiler});
  auto version = capture({compiler, "--version"});
  if (version.find("Apple metal version " KALANDRA_GPU_METAL_VERSION " ") == std::string::npos)
    throw std::runtime_error(
        "Apple Metal compiler version mismatch; required " KALANDRA_GPU_METAL_VERSION);
  return compiler;
#else
  return KALANDRA_GPU_METAL_COMPILER;
#endif
}
std::string platformSDK() {
#ifdef KALANDRA_INSTALLED_COMPILER
  auto sdk = capture({"/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-path"});
  if (!std::filesystem::path(sdk).is_absolute() || !std::filesystem::is_directory(sdk))
    throw std::runtime_error("Apple macOS SDK is unavailable");
  return sdk;
#else
  return KALANDRA_GPU_METAL_SDK;
#endif
}
std::string resource(const char *configured, bool include = false) {
#ifdef KALANDRA_INSTALLED_COMPILER
  auto path = resourceRoot() / (include ? std::filesystem::path("headers/metal")
                                        : std::filesystem::path(configured).filename());
  std::error_code error;
  bool present = include ? std::filesystem::is_directory(path, error)
                         : std::filesystem::is_regular_file(path, error);
  if (!present)
    throw std::runtime_error("installed Metal resource is missing: " + path.string());
  return path.string();
#else
  (void)include;
  return configured;
#endif
}
} // namespace
void buildMetalProgram(const std::filesystem::path &directory,
                       const std::map<std::string, std::string> &outputs,
                       const std::string &variant, kalandra::ArithmeticMode arithmetic) {
  const auto strict = resource(KALANDRA_GPU_STRICT);
  const auto strict64 = resource(KALANDRA_GPU_STRICT64);
  const auto extract = resource(KALANDRA_GPU_EXTRACT);
  const auto include = resource(KALANDRA_GPU_INCLUDE, true);
  const auto clang = tool(KALANDRA_GPU_CLANG, "clang++");
  const auto opt = tool(KALANDRA_GPU_OPT, "mlir-opt");
  const auto translate = tool(KALANDRA_GPU_TRANSLATE, "mlir-translate");
  const auto validate = tool(KALANDRA_GPU_VALIDATE, "spirv-val");
  const auto crossTool = tool(KALANDRA_GPU_CROSS, "spirv-cross");
  const auto metalCompiler = platformMetal();
  const auto sdk = platformSDK();
  const auto metal = resource(variant == "optimized"          ? KALANDRA_GPU_METAL_OPTIMIZED
                              : variant == "sanitized"        ? KALANDRA_GPU_METAL_SANITIZED
                              : variant == "thread-sanitized" ? KALANDRA_GPU_METAL_THREAD_SANITIZED
                                                              : KALANDRA_GPU_METAL_DEBUG);
  const auto core = resource(variant == "optimized"          ? KALANDRA_GPU_CORE_OPTIMIZED
                             : variant == "sanitized"        ? KALANDRA_GPU_CORE_SANITIZED
                             : variant == "thread-sanitized" ? KALANDRA_GPU_CORE_THREAD_SANITIZED
                                                             : KALANDRA_GPU_CORE_DEBUG);
  for (const auto &[name, contents] : outputs) {
    if (!name.starts_with("kernel") || !name.ends_with(".mlir"))
      continue;
    auto base = (directory / std::filesystem::path(name).stem()).string();
    const bool numeric = contents.find("@kalandra_numeric_v1_") != std::string::npos;
    std::vector<std::string> lowering{numeric ? strict64 : strict, base + ".mlir",
                                      base + ".lowered.mlir"};
    if (arithmetic != kalandra::ArithmeticMode::Software)
      lowering.push_back(arithmetic == kalandra::ArithmeticMode::Hardware
                             ? "--hardware"
                             : "--allow-software-fallback");
    run(std::move(lowering));
    run({opt, base + ".lowered.mlir", "--verify-each",
         "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-"
         "lower-abi-attrs,spirv-update-vce))",
         "-o", base + ".converted.mlir"});
    run({extract, base + ".converted.mlir", base + ".kernel.mlir", "--metal-compiler-rounding"});
    run({translate, "--no-implicit-module", "--serialize-spirv", base + ".kernel.mlir", "-o",
         base + ".spv"});
    run({validate, "--target-env", "vulkan1.0", base + ".spv"});
    std::vector<std::string> cross{crossTool,
                                   base + ".spv",
                                   "--msl",
                                   "--msl-version",
                                   numeric ? KALANDRA_GPU_TYPED_MSL_VERSION : "20200",
                                   "--output",
                                   base + ".metal"};
    if (contents.find("@kalandra_elementwise_v3(") != std::string::npos ||
        contents.find("// kalandra.numeric.indirect") != std::string::npos)
      cross.insert(cross.end(), {"--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
                                 "--msl-device-argument-buffer", "0",
                                 "--msl-force-active-argument-buffer-resources"});
    run(std::move(cross));
    if (!numeric && arithmetic != kalandra::ArithmeticMode::Software) {
      std::ofstream optimized(base + ".metal", std::ios::app);
      if (contents.find("@kalandra_reduce_sum(") != std::string::npos)
        optimized << hardwareReductionShader;
      if (contents.find("@kalandra_matmul(") != std::string::npos)
        optimized << hardwareMatmulShader << hardwareRegisterMatmulShader;
      if (!optimized)
        throw std::runtime_error("cannot emit hardware Metal entries");
    }
    run({metalCompiler, "-mmacosx-version-min=" KALANDRA_NATIVE_MACOS_MINIMUM, "-isysroot", sdk,
         std::string("-std=") + (numeric ? KALANDRA_GPU_TYPED_METAL_LANGUAGE : "macos-metal2.2"),
         "-fno-fast-math", "-ffp-contract=off",
         "-fmodules-cache-path=" + (directory / "module-cache").string(), "-c", base + ".metal",
         "-o", base + ".air"});
    run({metalCompiler, base + ".air", "-o", base + ".metallib"});
  }
  // Each entry is independently validated/lowered before offline linking.
  // Runtime loading verifies all entry interfaces before publishing the bundle.
  for (const auto &[name, contents] : outputs) {
    (void)contents;
    if (!name.starts_with("kernel") || !name.ends_with(".mlir"))
      continue;
    auto stem = std::filesystem::path(name).stem().string();
    if (!outputs.contains(stem + "-rows2.mlir"))
      continue;
    auto base = (directory / stem).string();
    run({metalCompiler, base + ".air", base + "-rows2.air", base + "-quad.air", "-o",
         base + ".metallib"});
  }
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  auto executable = directory / "program";
  if (auto error = llvm::sys::fs::createUniqueFile(executable.string() + ".tmp-%%%%%%", descriptor,
                                                   temporary))
    throw std::runtime_error(error.message());
  llvm::sys::fs::closeFile(descriptor);
  llvm::FileRemover cleanup(temporary);
  std::vector<std::string> command{clang,
                                   "-std=c++23",
                                   "-Wall",
                                   "-Wextra",
                                   "-Werror",
                                   "-ffp-contract=off",
                                   "-isysroot",
                                   sdk,
                                   "-I",
                                   include,
                                   "-mmacosx-version-min=" KALANDRA_NATIVE_MACOS_MINIMUM};
#ifdef KALANDRA_INSTALLED_COMPILER
  const auto clangResources = resourceRoot().parent_path() / "toolchain/clang";
  if (clang != KALANDRA_GPU_CLANG) {
    if (!std::filesystem::is_directory(clangResources / "include") ||
        !std::filesystem::is_directory(clangResources / "lib"))
      throw std::runtime_error("bundled Clang resources are missing");
    command.push_back("-resource-dir=" + clangResources.string());
  }
#endif
  if (variant == "optimized") {
    command.push_back("-O2");
  } else if (variant == "sanitized") {
    command.insert(command.end(), {"-O1", "-g", "-fsanitize=address,undefined",
                                   "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"});
  } else if (variant == "thread-sanitized") {
    command.insert(command.end(), {"-O1", "-g", "-fsanitize=thread", "-fno-omit-frame-pointer"});
  } else
    command.push_back("-O0");
  if (outputs.contains("cpu.ll")) {
    command.push_back("-Wno-override-module");
    command.push_back((directory / "cpu.ll").string());
  }
  command.insert(command.end(),
                 {(directory / "host.cpp").string(), metal, core, "-framework", "Foundation",
                  "-framework", "Metal", "-o", std::string(temporary)});
  run(std::move(command));
  std::filesystem::rename(std::string(temporary), executable);
}
