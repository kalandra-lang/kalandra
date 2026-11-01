#include "VulkanBuild.h"
#include "VulkanBuildConfig.h"
#include "BuiltinShaders.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void run(std::vector<std::string> command) {
  std::vector<llvm::StringRef> arguments(command.begin(), command.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(command[0], arguments, {}, {}, 0, 0, &error))
    throw std::runtime_error("Vulkan build stage failed: " + command[0] + " " + error);
}
#ifdef KALANDRA_INSTALLED_COMPILER
std::filesystem::path resourceRoot() {
  static int anchor;
  const auto raw = llvm::sys::fs::getMainExecutable(nullptr, &anchor);
  std::error_code error;
  auto executable = std::filesystem::canonical(std::filesystem::path(std::u8string(raw.begin(), raw.end())), error);
  if (error)
    throw std::runtime_error("cannot locate installed Vulkan compiler executable");
  return executable.parent_path() / KALANDRA_RUNTIME_RELATIVE_DIR;
}
#endif
std::string tool(const char *configured, const char *name) {
#ifdef KALANDRA_INSTALLED_COMPILER
  (void)configured;
  auto path = (resourceRoot() / "../../../libexec/kalandra" / name).lexically_normal();
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("bundled Vulkan tool is missing: " + path.string());
  return path.string();
#else
  (void)name;
  return configured;
#endif
}
std::string resource(const char *configured, const char *headers = nullptr) {
#ifdef KALANDRA_INSTALLED_COMPILER
  auto path = resourceRoot() / (headers ? std::filesystem::path(headers)
                                      : std::filesystem::path(configured).filename());
  const bool present = headers ? std::filesystem::is_directory(path)
                               : std::filesystem::is_regular_file(path);
  if (!present)
    throw std::runtime_error("installed Vulkan resource is missing: " + path.string());
  return path.string();
#else
  (void)headers;
  return configured;
#endif
}
void transform(const std::filesystem::path &path, const std::string &contents) {
  constexpr std::string_view header = "kalandra-vulkan-transform-v1\nkind=";
  if (!contents.starts_with(header) || !contents.ends_with('\n'))
    throw std::runtime_error("invalid Vulkan transform descriptor");
  const auto split = contents.find("\ntype=", header.size());
  if (split == std::string::npos)
    throw std::runtime_error("invalid Vulkan transform type");
  const auto name = "transforms/" + contents.substr(header.size(), split - header.size()) + "-" +
                    contents.substr(split + 6, contents.size() - split - 7) + ".spv";
  auto words = kalandra::runtime::vulkan::builtinShader(name);
  if (words.empty())
    throw std::runtime_error("unsupported Vulkan transform resource");
  std::ofstream output(path, std::ios::binary);
  for (auto word : words)
    for (unsigned byte = 0; byte < 4; ++byte)
      output.put(static_cast<char>((word >> (byte * 8)) & 255));
  output.close();
  if (!output)
    throw std::runtime_error("cannot write Vulkan transform module");
}
} // namespace
void buildVulkanProgram(const std::filesystem::path &directory,
                        const std::map<std::string, std::string> &outputs,
                        const std::string &variant, kalandra::ArithmeticMode arithmetic,
                        kalandra::MatmulPolicy matmul) {
  const auto lower = tool(KALANDRA_VULKAN_LOWER, "kalandra-vulkan-lower" KALANDRA_VULKAN_EXE_SUFFIX);
  const auto opt = tool(KALANDRA_VULKAN_OPT, "mlir-opt" KALANDRA_VULKAN_EXE_SUFFIX);
  const auto extract = tool(KALANDRA_VULKAN_EXTRACT, "kalandra-vulkan-extract" KALANDRA_VULKAN_EXE_SUFFIX);
  const auto translate = tool(KALANDRA_VULKAN_TRANSLATE, "mlir-translate" KALANDRA_VULKAN_EXE_SUFFIX);
  const auto validate = tool(KALANDRA_VULKAN_VALIDATE, "spirv-val" KALANDRA_VULKAN_EXE_SUFFIX);
  const auto clang = tool(KALANDRA_VULKAN_CLANG, "clang++" KALANDRA_VULKAN_EXE_SUFFIX);
  std::vector<std::string> resourceFlags;
#ifdef KALANDRA_INSTALLED_COMPILER
  const auto clangResources = resourceRoot().parent_path() / "toolchain/clang";
  if (!std::filesystem::is_directory(clangResources / "include") ||
      !std::filesystem::is_directory(clangResources / "lib"))
    throw std::runtime_error("bundled Vulkan Clang resources are missing");
  resourceFlags.push_back("-resource-dir=" + clangResources.string());
#endif
  for (const auto &[name, contents] : outputs) {
    if (name.ends_with(".vulkan-transform")) {
      auto target = directory / name;
      target.replace_extension(".spv");
      transform(target, contents);
    } else if (name.starts_with("kernel") && name.ends_with(".mlir")) {
      auto target = directory / name;
      target.replace_extension();
      const auto compile = [&](const std::string &base, const char *policy) {
        run({lower, (directory / name).string(), base + ".lowered.mlir", policy});
        run({opt, base + ".lowered.mlir", "--verify-each",
             "--pass-pipeline=builtin.module(canonicalize,cse,convert-gpu-to-spirv,spirv.module(spirv-lower-abi-attrs,spirv-update-vce))",
             "-o", base + ".converted.mlir"});
        run({extract, base + ".converted.mlir", base + ".kernel.mlir"});
        run({translate, "--no-implicit-module", "--serialize-spirv",
             base + ".kernel.mlir", "-o", base + ".spv"});
        run({validate, "--target-env", "vulkan1.0", base + ".spv"});
      };
      compile(target.string(), arithmetic == kalandra::ArithmeticMode::Software ? "software" : "hardware");
      if (arithmetic == kalandra::ArithmeticMode::HardwareWithSoftwareFallback)
        compile(target.string() + ".spv.software", "hardware-with-software-fallback");
      const auto marker = outputs.find(target.filename().string() + ".vulkan-matmul");
      if (matmul == kalandra::MatmulPolicy::Optimized && arithmetic != kalandra::ArithmeticMode::Software &&
          marker != outputs.end()) {
        if (marker->second != "kalandra-vulkan-matmul-v1\ntype=float32\n")
          throw std::runtime_error("invalid Vulkan matmul descriptor");
        std::filesystem::copy_file(target.string() + ".spv", target.string() + ".spv.fixed-tree.spv",
                                   std::filesystem::copy_options::overwrite_existing);
        auto words = kalandra::runtime::vulkan::builtinShader("mul_mm.spv");
        std::ofstream output(target.string() + ".spv", std::ios::binary);
        for (auto word : words)
          for (unsigned byte = 0; byte < 4; ++byte)
            output.put(static_cast<char>((word >> (byte * 8)) & 255));
        output.close();
        if (!output)
          throw std::runtime_error("cannot write optimized Vulkan matmul module");
      }
    }
  }
  const bool sanitized = variant == "sanitized";
  const auto optimization = sanitized ? "-O1" : variant == "optimized" ? "-O2" : "-O0";
  const std::vector<std::string> sanitizerFlags = sanitized
      ? std::vector<std::string>{"-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"}
      : std::vector<std::string>{};
  const auto compile = [&](std::vector<std::string> command) {
    command.insert(command.end(), sanitizerFlags.begin(), sanitizerFlags.end());
    command.insert(command.end(), resourceFlags.begin(), resourceFlags.end());
    run(std::move(command));
  };
  std::vector<std::string> link{clang};
  link.insert(link.end(), resourceFlags.begin(), resourceFlags.end());
  if (outputs.contains("cpu.ll")) {
    compile({clang, optimization, "-c", (directory / "cpu.ll").string(),
         "-o", (directory / "cpu.o").string()});
    link.push_back((directory / "cpu.o").string());
  }
  compile({clang, "-std=c++23", optimization, "-Wall", "-Wextra", "-Werror",
       "-I" + resource(KALANDRA_VULKAN_INCLUDE, "headers/vulkan"),
       "-I" + resource(KALANDRA_VULKAN_PUBLIC_INCLUDE, "headers/include"),
       "-c", (directory / "host.cpp").string(), "-o", (directory / "host.o").string()});
  link.push_back((directory / "host.o").string());
  const std::vector<std::string> normalLibraries{KALANDRA_VULKAN_LANGUAGE, KALANDRA_VULKAN_ASYNC,
                              KALANDRA_VULKAN_PROGRAM, KALANDRA_VULKAN_DRIVER,
                              KALANDRA_VULKAN_CORE};
#if KALANDRA_VULKAN_HAS_ASAN
  const std::vector<std::string> sanitizedLibraries{KALANDRA_VULKAN_LANGUAGE_SANITIZED,
      KALANDRA_VULKAN_ASYNC_SANITIZED, KALANDRA_VULKAN_PROGRAM_SANITIZED,
      KALANDRA_VULKAN_DRIVER_SANITIZED, KALANDRA_VULKAN_CORE_SANITIZED};
  const auto &libraries = sanitized ? sanitizedLibraries : normalLibraries;
#else
  if (sanitized)
    throw std::runtime_error("Vulkan sanitizer runtime unavailable");
  const auto &libraries = normalLibraries;
#endif
  for (const auto &library : libraries)
    link.push_back(resource(library.c_str()));
  for (const auto *library : kalandraVulkanHostLibraries)
    if (library) link.emplace_back(library);
  link.insert(link.end(), sanitizerFlags.begin(), sanitizerFlags.end());
#ifndef _WIN32
  link.emplace_back("-pthread");
#endif
  link.insert(link.end(), {"-o", (directory / ("program" KALANDRA_VULKAN_EXE_SUFFIX)).string()});
  run(std::move(link));
}
