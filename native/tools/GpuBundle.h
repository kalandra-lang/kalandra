#pragma once
#include "kalandra/CudaSource.h"
#include "kalandra/VulkanSource.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace kalandra::cli {
// Build both device modules in private directories. Publish only the linked
// application, its device code and the notices once both compilers succeed.
inline int compileGpuBundle(std::vector<char *> arguments, Output feedback) {
  namespace fs = std::filesystem;
  std::size_t outputArg = 0;
  bool build = false;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string_view option = arguments[i];
    if (option == "-o" || option == "--arithmetic" || option == "--matmul-policy" ||
        option == "--package" || option == "--project" || option == "--test-entry") {
      if (++i == arguments.size())
        return 2;
      if (option == "-o") {
        if (outputArg)
          return 2;
        outputArg = i;
      }
    } else if (option == "--build")
      build = true;
  }
  if (!outputArg || !*arguments[outputArg]) {
    std::cerr << "GPU bundles require -o <new directory>\n";
    return 2;
  }
  char buildFlag[] = "--build";
  if (!build)
    arguments.push_back(buildFlag);
  try {
    const auto output = fs::absolute(arguments[outputArg]).lexically_normal();
    if (fs::symlink_status(output).type() != fs::file_type::not_found)
      throw std::runtime_error("bundle output already exists; choose a new directory");
    fs::create_directories(output.parent_path());
    llvm::SmallString<256> temporary;
    if (auto error = llvm::sys::fs::createUniqueDirectory(
            (output.parent_path() / ".kalandra-bundle").string(), temporary))
      throw std::system_error(error);
    struct Stage {
      fs::path directory;
      ~Stage() {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
      }
    } stage{temporary.str().str()};
    auto cuda = (stage.directory / "cuda").string(), vulkan = (stage.directory / "vulkan").string();
    arguments[outputArg] = cuda.data();
    if (int result =
            compileCudaCommand(int(arguments.size()), arguments.data(), true, feedback, true))
      return result;
    arguments[outputArg] = vulkan.data();
    if (int result =
            compileVulkanCommand(int(arguments.size()), arguments.data(), true, feedback, true))
      return result;
    const auto bundle = stage.directory / "application";
    fs::create_directory(bundle);
    std::ofstream notices(bundle / "THIRD_PARTY_NOTICES.txt", std::ios::binary);
    notices.exceptions(std::ios::badbit | std::ios::failbit);
    for (auto &backend : {cuda, vulkan}) {
      std::ifstream notice(fs::path(backend) / "THIRD_PARTY_NOTICES.txt", std::ios::binary);
      notices << notice.rdbuf() << '\n';
      for (auto &item : fs::directory_iterator(backend)) {
        const auto &path = item.path();
        if (path.extension() == ".spv" || path.extension() == ".ptx" ||
            path.filename() == "program" || path.filename() == "program.exe")
          fs::rename(path, bundle / path.filename());
      }
    }
    notices.close();
    fs::rename(bundle, output);
    if (!feedback.silent)
      std::cout << "Built CUDA + Vulkan bundle " << output.string() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "GPU bundle: " << e.what() << '\n';
    return 1;
  }
}
} // namespace kalandra::cli
