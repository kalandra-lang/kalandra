#include "Build.h"
#include "CudaBuildConfig.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include <fstream>
#include <stdexcept>
#include <vector>
namespace {
#ifdef KALANDRA_INSTALLED_COMPILER
std::filesystem::path resourceRoot() {
  static int anchor;
  const auto raw = llvm::sys::fs::getMainExecutable(nullptr, &anchor);
  std::error_code error;
  const auto executable = std::filesystem::canonical(
      std::filesystem::path(std::u8string(raw.begin(), raw.end())), error);
  if (error)
    throw std::runtime_error("cannot locate installed CUDA compiler executable");
  return (executable.parent_path() / KALANDRA_RUNTIME_RELATIVE_DIR).lexically_normal();
}
#endif
std::string tool(const char *configured, const char *name) {
#ifdef KALANDRA_INSTALLED_COMPILER
  (void)configured;
  const auto path = (resourceRoot() / KALANDRA_CUDA_TOOLS_RELATIVE / name).lexically_normal();
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("bundled CUDA tool is missing: " + path.string());
  return path.string();
#else
  (void)name;
  return configured;
#endif
}
std::string resource(const char *configured, const char *relative = nullptr,
                     bool directory = false) {
#ifdef KALANDRA_INSTALLED_COMPILER
  const auto path = resourceRoot() / (relative ? std::filesystem::path(relative)
                                               : std::filesystem::path(configured).filename());
  const bool present =
      directory ? std::filesystem::is_directory(path) : std::filesystem::is_regular_file(path);
  if (!present)
    throw std::runtime_error("installed CUDA resource is missing: " + path.string());
  return path.string();
#else
  (void)relative;
  (void)directory;
  return configured;
#endif
}
void run(std::vector<std::string> command) {
  std::vector<llvm::StringRef> arguments(command.begin(), command.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(command[0], arguments, {}, {}, 0, 0, &error))
    throw std::runtime_error("CUDA build stage failed: " + command[0] + " " + error);
}
} // namespace
void buildCudaProgram(const std::filesystem::path &directory,
                      const std::map<std::string, std::string> &outputs, const std::string &variant,
                      kalandra::ArithmeticMode arithmetic, kalandra::MatmulPolicy matmul) {
  if ((matmul != kalandra::MatmulPolicy::FixedTree &&
       matmul != kalandra::MatmulPolicy::Optimized) ||
      (variant != "debug" && variant != "optimized"))
    throw std::runtime_error("unsupported CUDA build policy");
  const auto lower = tool(KALANDRA_CUDA_LOWER, "kalandra-cuda-lower");
  const auto clang = tool(KALANDRA_CUDA_CLANG, "clang++");
  std::vector<std::string> clangFlags;
#ifdef KALANDRA_INSTALLED_COMPILER
  const auto clangResources = resourceRoot().parent_path() / "toolchain/clang";
  if (!std::filesystem::is_directory(clangResources / "include") ||
      !std::filesystem::is_directory(clangResources / "lib"))
    throw std::runtime_error("bundled CUDA Clang resources are missing");
  clangFlags.push_back("-resource-dir=" + clangResources.string());
#endif
  const auto compile = [&](std::vector<std::string> command) {
    command.insert(command.end(), clangFlags.begin(), clangFlags.end());
    run(std::move(command));
  };
  for (const auto &[name, contents] : outputs) {
    auto target = directory / name;
    if (name.ends_with(".cuda-transform")) {
      constexpr std::string_view header = "kalandra-cuda-transform-v1\nkind=";
      if (!contents.starts_with(header) || !contents.ends_with('\n'))
        throw std::runtime_error("invalid CUDA transform descriptor");
      auto split = contents.find("\ntype=", header.size());
      if (split == std::string::npos)
        throw std::runtime_error("invalid CUDA transform type");
      auto kind = contents.substr(header.size(), split - header.size());
      auto numeric = contents.substr(split + 6, contents.size() - split - 7);
      if ((kind != "transpose" && kind != "softmaxRows" && kind != "rmsNormRows") ||
          (numeric != "int8" && numeric != "uint8" && numeric != "int16" && numeric != "uint16" &&
           numeric != "int32" && numeric != "uint32" && numeric != "int64" && numeric != "uint64" &&
           numeric != "float32" && numeric != "float64"))
        throw std::runtime_error("unsupported CUDA transform descriptor");
      target.replace_extension(".ptx");
      std::filesystem::copy_file(
          std::filesystem::path(resource(KALANDRA_CUDA_TRANSFORMS, "cuda/transforms", true)) /
              (kind + "-" + numeric + ".ptx"),
          target, std::filesystem::copy_options::overwrite_existing);
    } else if (name.starts_with("kernel") && name.ends_with(".mlir")) {
      auto ptx = target;
      ptx.replace_extension(".ptx");
      run({lower, target.string(), ptx.string(),
           arithmetic == kalandra::ArithmeticMode::Software ? "software" : "hardware", "sm_80"});
      if (arithmetic == kalandra::ArithmeticMode::HardwareWithSoftwareFallback)
        run({lower, target.string(), ptx.string() + ".software.ptx",
             "hardware-with-software-fallback", "sm_80"});
      const auto marker = outputs.find(target.stem().string() + ".cuda-matmul");
      if (matmul == kalandra::MatmulPolicy::Optimized &&
          arithmetic != kalandra::ArithmeticMode::Software && marker != outputs.end()) {
        if (marker->second != "kalandra-cuda-matmul-v1\ntype=float32\n")
          throw std::runtime_error("invalid CUDA matmul descriptor");
        std::filesystem::copy_file(ptx, ptx.string() + ".fixed-tree.ptx",
                                   std::filesystem::copy_options::overwrite_existing);
        std::filesystem::rename(ptx.string() + ".json", ptx.string() + ".fixed-tree.ptx.json");
        std::filesystem::copy_file(resource(KALANDRA_CUDA_MATMUL, "cuda/kernels/matmul-warp64.ptx"),
                                   ptx, std::filesystem::copy_options::overwrite_existing);
      }
    }
  }
  const auto optimization = variant == "optimized" ? "-O2" : "-O0";
  std::vector<std::string> link{clang};
  link.insert(link.end(), clangFlags.begin(), clangFlags.end());
  if (outputs.contains("cpu.ll")) {
    compile({clang, optimization, "-ffp-contract=off", "-c", (directory / "cpu.ll").string(), "-o",
             (directory / "cpu.o").string()});
    link.push_back((directory / "cpu.o").string());
  }
  compile({clang, "-std=c++23", optimization, "-ffp-contract=off", "-Wall", "-Wextra", "-Werror",
           "-I" + resource(KALANDRA_CUDA_INCLUDE, "headers/cuda", true),
           "-I" + resource(KALANDRA_CUDA_PUBLIC_INCLUDE, "headers/include", true), "-c",
           (directory / "host.cpp").string(), "-o", (directory / "host.o").string()});
  link.push_back((directory / "host.o").string());
  for (const char *library :
       {KALANDRA_CUDA_LANGUAGE, KALANDRA_CUDA_PROGRAM, KALANDRA_CUDA_DRIVER, KALANDRA_CUDA_CORE})
    link.emplace_back(resource(library));
  link.insert(link.end(), {"-ldl", "-pthread", "-o", (directory / "program").string()});
  run(std::move(link));
}
