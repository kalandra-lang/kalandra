#include "Supervisor.h"
#include "VulkanBuild.h"
#include "VulkanBuildConfig.h"
#include "kalandra/VulkanSource.h"
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
int kalandra::compileVulkanCommand(int argc, char **argv) {
  const auto usage = [] {
    std::cerr << "usage: kalandra --target vulkan <source.k | --project "
                 "manifest.toml> -o directory "
                 "[--arithmetic hardware|software|hardware-with-software-fallback] "
                 "[--matmul-policy fixed-tree] "
                 "[--package name=path.k ...] [--build "
                 "[-O0|-O2|--sanitize]]\n";
    return 2;
  };
  bool build = false, testCpuExports = false;
  ArithmeticMode arithmetic = ArithmeticMode::Hardware;
  bool selectedArithmetic = false;
  MatmulPolicy matmul = MatmulPolicy::FixedTree;
  bool selectedMatmul = false;
  std::string variant = "debug", sourcePath, projectPath;
  std::filesystem::path directory;
  std::optional<NativeProject> project;
  std::vector<ModulePackage> packages;
  bool selected = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view option = argv[i];
    if (option == "-o" || option == "--project" || option == "--package") {
      if (++i >= argc || std::string_view(argv[i]).empty())
        return usage();
      if (option == "-o") {
        if (!directory.empty())
          return usage();
        directory = argv[i];
      } else if (option == "--project") {
        if (!projectPath.empty())
          return usage();
        projectPath = argv[i];
      } else {
        auto package = parseModulePackage(argv[i]);
        if (!package) {
          std::cerr << package.error() << '\n';
          return 2;
        }
        packages.push_back(std::move(*package));
      }
    } else if (option == "--arithmetic" && !selectedArithmetic) {
      if (++i >= argc)
        return usage();
      auto parsed = parseArithmeticMode(argv[i]);
      if (!parsed)
        return usage();
      arithmetic = *parsed;
      selectedArithmetic = true;
    } else if (option == "--matmul-policy" && !selectedMatmul) {
      if (++i >= argc)
        return usage();
      auto parsed = parseMatmulPolicy(argv[i]);
      if (!parsed)
        return usage();
      matmul = *parsed;
      selectedMatmul = true;
    } else if (option == "--test-cpu-exports" && !testCpuExports) {
      testCpuExports = true;
    } else if (option == "--build" && !build)
      build = true;
    else if (!selected && (option == "-O0" || option == "-O2" || option == "--sanitize" ||
                           option == "--thread-sanitize")) {
      selected = true;
      variant = option == "-O2"          ? "optimized"
                : option == "--sanitize" ? "sanitized"
                : option == "-O0"        ? "debug"
                                         : "thread-sanitized";
    } else if (!option.starts_with('-') && sourcePath.empty())
      sourcePath = option;
    else
      return usage();
  }
  if (directory.empty() || (selected && !build) || (!sourcePath.empty() && !projectPath.empty()))
    return usage();
  if ((variant == "sanitized" && !KALANDRA_VULKAN_HAS_ASAN) || variant == "thread-sanitized") {
    std::cerr << "Vulkan sanitizer packages are not integrated yet\n";
    return 2;
  }
  if (matmul != MatmulPolicy::FixedTree) {
    std::cerr << "optimized Vulkan matmul routing is not integrated yet; use --matmul-policy fixed-tree\n";
    return 2;
  }
  if (!projectPath.empty()) {
    auto loaded = loadNativeProject(projectPath);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    project = std::move(*loaded);
    sourcePath = project->entry;
    packages.insert(packages.end(), project->packages.begin(), project->packages.end());
  }
  if (sourcePath.empty())
    return usage();
  std::optional<InstalledNativeProject> managed;
  if (project && !project->dependencies.empty()) {
    auto loaded = loadInstalledNativeProject(*project);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    managed = std::move(*loaded);
    managed->nodes[0].localPackages = packages;
  }
  auto parsed =
      managed ? loadPackageModules(managed->nodes) : loadModules(sourcePath, {}, {}, packages);
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  auto source = parsed->module.source;
  try {
    if (!parsed->ok()) {
      for (auto &d : parsed->diagnostics)
        std::cerr << renderDiagnostic(*source, d);
      return 1;
    }
    auto emitted = emitGpuSupervisor(std::move(parsed->module), arithmetic, testCpuExports, GpuBackend::Vulkan);
    if (!emitted) {
      std::cerr << renderDiagnostic(*source, emitted.error());
      return 1;
    }
    auto outputs = std::move(*emitted);
    outputs["manifest.txt"] += "matmul_policy=" + std::string(matmulPolicyName(matmul)) + "\n";
    outputs["THIRD_PARTY_NOTICES.txt"] = kalandraVulkanNotice;
    if (variant == "sanitized" && outputs.contains("cpu.ll")) {
      llvm::LLVMContext context;
      llvm::SMDiagnostic diagnostic;
      auto module = llvm::parseAssemblyString(outputs.at("cpu.ll"), diagnostic, context);
      if (!module)
        throw std::runtime_error("cannot prepare CPU sanitizer attributes");
      for (auto &function : *module)
        if (!function.isDeclaration())
          function.addFnAttr(llvm::Attribute::SanitizeAddress);
      outputs["cpu.ll"].clear();
      llvm::raw_string_ostream stream(outputs["cpu.ll"]);
      module->print(stream, nullptr);
    }
    outputs["manifest.txt"] += "host_instrumentation=" +
        std::string(variant == "sanitized" ? "address,undefined" : "none") + "\n";
    std::vector<std::string> paths;
    for (const auto &[name, contents] : outputs) {
      (void)contents;
      paths.push_back(name);
      if (build && name.starts_with("kernel") && name.ends_with(".mlir")) {
        auto base = std::filesystem::path(name).stem().string();
        for (const auto *suffix : {".lowered.mlir", ".converted.mlir", ".kernel.mlir", ".spv"}) {
          paths.push_back(base + suffix);
          if (arithmetic == ArithmeticMode::HardwareWithSoftwareFallback)
            paths.push_back(base + ".spv.software" + suffix);
        }
      }
      if (build && name.ends_with(".vulkan-transform"))
        paths.push_back(std::filesystem::path(name).stem().string() + ".spv");
    }
    if (build) {
      paths.push_back("program" + std::string(KALANDRA_VULKAN_EXE_SUFFIX));
      paths.push_back("host.o");
      if (outputs.contains("cpu.ll")) paths.push_back("cpu.o");
    }
    for (const auto &name : paths) {
      auto target = directory / name;
      if (project) {
        auto allowed = protectNativePackageOutput(*project, target.string());
        if (!allowed)
          throw std::runtime_error(allowed.error());
      }
      std::error_code ec;
      auto protect = [&](const std::filesystem::path &input) {
        std::error_code first, second;
        const bool equivalent = std::filesystem::equivalent(target, input, ec);
        const auto a = std::filesystem::weakly_canonical(target, first);
        const auto b = std::filesystem::weakly_canonical(input, second);
        if (equivalent || (!first && !second && a == b))
          throw std::runtime_error("source and output paths must be distinct");
      };
      protect(source->path());
      for (const auto &part : source->parts())
        protect(part.source->path());
      if (project) {
        protect(project->manifest);
        if (project->libraryEntry)
          protect(*project->libraryEntry);
      }
      if (std::filesystem::is_symlink(std::filesystem::symlink_status(target)))
        throw std::runtime_error("refusing symlink output");
    }
    std::filesystem::create_directories(directory);
    for (auto &[name, contents] : outputs) {
      llvm::SmallString<256> temporary;
      int descriptor = -1;
      auto target = directory / name;
      if (auto ec = llvm::sys::fs::createUniqueFile(target.string() + ".tmp-%%%%%%", descriptor,
                                                    temporary))
        throw std::runtime_error(ec.message());
      llvm::FileRemover remove(temporary);
      llvm::raw_fd_ostream file(descriptor, true);
      file << contents;
      file.close();
      if (file.has_error())
        throw std::runtime_error("cannot write compiler output");
      if (auto error = llvm::sys::fs::rename(temporary, target.string()))
        throw std::runtime_error(error.message());
    }
    if (build)
      buildVulkanProgram(directory, outputs, variant, arithmetic);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Vulkan source compilation: " << e.what() << '\n';
  }
  return 1;
}
