#include "../lib/NativeSanitizerFrames.h"
#include "../lib/OrdinaryMain.h"
#include "BuildTool.h"
#include "CExport.h"
#include "CrossToolchain.h"
#include "kalandra/Application.h"
#include "kalandra/HeaderImport.h"

// LLVM COFF declarations must precede Windows SDK macros from platform I/O.
#include "../lib/PlatformFileSystem.h"
#include "kalandra/Backend.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/Version.h"
#ifdef KALANDRA_HAS_METAL_SOURCE
#include "kalandra/MetalSource.h"
#endif
#ifdef KALANDRA_HAS_VULKAN_SOURCE
#include "kalandra/VulkanSource.h"
#endif
#ifdef KALANDRA_HAS_CUDA_SOURCE
#include "kalandra/CudaSource.h"
#endif
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Process.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#if defined(KALANDRA_HAS_CUDA_SOURCE) && defined(KALANDRA_HAS_VULKAN_SOURCE)
#include "GpuBundle.h"
#endif
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

namespace {
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr auto nativeCpuTarget = "cpu/arm64";
#else
constexpr auto nativeCpuTarget = "cpu/x86_64";
#endif

bool invalidModule(const llvm::Module &module, kalandra::cli::BuildProgress &progress) {
  std::string diagnostic;
  llvm::raw_string_ostream buffer(diagnostic);
  if (!llvm::verifyModule(module, &buffer))
    return false;
  progress.diagnostic() << diagnostic;
  return true;
}

constexpr std::string_view usage =
    "usage: kalandra <source.k | --project manifest.toml> -o <output> [options]\n"
    "\nProject commands\n"
    "  new <directory>                     Create a project\n"
    "  install --project <manifest>        Install dependencies [--frozen] [--offline]\n"
    "  update <name>... --project <manifest>  Update direct dependencies\n"
    "  remove <alias> [--project <manifest>]  Remove a direct dependency\n"
    "\nBuild options\n"
    "  --target cpu|metal|vulkan|cuda      Select the build backend\n"
    "  --target cuda,vulkan                Bundle both GPU backends\n"
    "  -O0, -O1, -O2, -O3                  Select the optimization level\n"
    "  --package name=path.k               Map a local source library\n"
    "  --emit-mlir <file>, --emit-llvm <file>  Write intermediate code\n"
    "  --arithmetic hardware|software|hardware-with-software-fallback\n"
    "  --matmul-policy optimized|fixed-tree\n"
    "  --sanitize, --thread-sanitize\n"
    "  --platform <os-architecture> | --toolchain <profile.json>\n"
    "  --sysroot <path>, --gcc-directory <path>, --library-directory <path>\n"
    "\nOutput options\n"
    "  --emit object|static|shared|c-header  Build a native library or header\n"
    "  --silent                            Hide routine messages and progress\n"
    "  --debug                             Write build timings and context to stderr\n"
    "  --version                           Print the toolchain version\n";
struct LinkDirectory {
  std::filesystem::path path;
  ~LinkDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
bool write(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  return bool(output);
}
} // namespace
int main(int argc, char **argv) {
  const auto feedback = kalandra::cli::Output::take(argc, argv,
      {"-o", "--emit", "--header", "--project", "--package", "--emit-mlir", "--emit-llvm", "--test-entry",
       "--arithmetic", "--matmul-policy", "--platform", "--target", "--toolchain", "--sysroot",
       "--gcc-directory", "--library-directory"});
  if (kalandra::printVersion(argc, argv, "kalandra"))
    return 0;
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (argc >= 2 &&
      (std::string_view(argv[1]) == "remove" || std::string_view(argv[1]) == "--remove")) {
    constexpr auto usage = "usage: kalandra remove <alias> [--project manifest.toml]\n";
    if (argc == 3 && std::string_view(argv[2]) == "--help") {
      std::cout << usage;
      return 0;
    }
    std::string_view alias;
    std::optional<std::string> manifest;
    for (int i = 2; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--project" && !manifest && i + 1 < argc && argv[i + 1][0] &&
          argv[i + 1][0] != '-')
        manifest = argv[++i];
      else if (!arg.empty() && !arg.starts_with('-') && alias.empty())
        alias = arg;
      else {
        std::cerr << usage;
        return 2;
      }
    }
    if (alias.empty()) {
      std::cerr << usage;
      return 2;
    }
    feedback.trace("Removing " + std::string(alias) + " from", manifest.value_or("kalandra.toml"));
    auto removed = kalandra::removeNativeDependency(manifest.value_or("kalandra.toml"), alias);
    if (!removed) {
      std::cerr << removed.error() << '\n';
      return 1;
    }
    if (!feedback.silent)
      std::cout << "Removed dependency '" << alias << "'.\n";
    return 0;
  }
  if (argc >= 2 &&
      (std::string_view(argv[1]) == "install" || std::string_view(argv[1]) == "update")) {
    const bool update = std::string_view(argv[1]) == "update";
    std::string manifest;
    kalandra::PackageInstallOptions options;
    bool invalid = false;
    for (int i = 2; i < argc; ++i) {
      const std::string_view argument = argv[i];
      if (argument == "--project" && manifest.empty() && i + 1 < argc &&
          !std::string_view(argv[i + 1]).starts_with("--"))
        manifest = argv[++i];
      else if (!update && argument == "--frozen" && !options.frozen)
        options.frozen = true;
      else if (!update && argument == "--offline" && !options.offline)
        options.offline = true;
      else if (update && !argument.starts_with('-'))
        options.updateRoots.emplace_back(argument);
      else
        invalid = true;
    }
    if (invalid || manifest.empty() || (update && options.updateRoots.empty())) {
      std::cerr
          << (update ? "usage: kalandra update name [name ...] --project manifest.toml\n"
                     : "usage: kalandra install --project manifest.toml [--frozen] [--offline]\n");
      return 2;
    }
    if (feedback.debug) {
      options.trace = [&](std::string_view step, std::string_view detail) { feedback.trace(step, detail); };
      feedback.trace(update ? "Updating" : "Installing", manifest);
    }
    auto installed = kalandra::installNativeProject(manifest, {}, options);
    if (!installed) {
      std::cerr << installed.error() << '\n';
      return 1;
    }
    if (!feedback.silent)
      std::cout << (update ? "Updated native dependency graph.\n"
                         : "Installed native dependency graph.\n");
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "new") {
    if (argc != 3 || std::string_view(argv[2]) == "--help") {
      std::cerr << "usage: kalandra new directory\n";
      return argc == 3 ? 0 : 2;
    }
    feedback.trace("Creating project", argv[2]);
    auto created = kalandra::createNativeProject(argv[2]);
    if (!created) {
      std::cerr << created.error() << '\n';
      return 1;
    }
    if (!feedback.silent)
      std::cout << "Created native project at " << argv[2]
              << "\nCheck and build its kalandra.toml with --project.\n";
    return 0;
  }
  std::vector<char *> filtered{argv[0]};
  std::string target = "cpu", toolchainPath, platform;
  llvm::json::Object platformOptions;
  bool selectedTarget = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--toolchain") {
      if (!toolchainPath.empty() || i + 1 >= argc || std::string_view(argv[i + 1]).empty() ||
          std::string_view(argv[i + 1]).starts_with("--")) {
        std::cerr << "one complete --toolchain option is required\n";
        return 2;
      }
      toolchainPath = argv[++i];
    } else if (option == "--platform" || option == "--sysroot" || option == "--gcc-directory" ||
               option == "--library-directory") {
      if (i + 1 >= argc || std::string_view(argv[i + 1]).empty() ||
          std::string_view(argv[i + 1]).starts_with("--")) {
        std::cerr << "incomplete CPU platform option: " << option << '\n';
        return 2;
      }
      const std::string value = argv[++i];
      if (option == "--platform") {
        if (!platform.empty()) {
          std::cerr << "one --platform is allowed\n";
          return 2;
        }
        platform = value;
      } else if (option == "--library-directory") {
        if (!platformOptions.get("libraryDirectories"))
          platformOptions["libraryDirectories"] = llvm::json::Array{};
        platformOptions.getArray("libraryDirectories")->push_back(value);
      } else {
        const auto key = option == "--sysroot" ? "sysroot" : "gccDirectory";
        if (platformOptions.get(key)) {
          std::cerr << "duplicate CPU SDK option\n";
          return 2;
        }
        platformOptions[key] = value;
      }
    } else if (option == "--target") {
      if (selectedTarget || i + 1 >= argc) {
        std::cerr << "one complete --target option is required\n";
        return 2;
      }
      selectedTarget = true;
      target = argv[++i];
    } else {
      filtered.push_back(argv[i]);
      if ((option == "-o" || option == "--emit-mlir" || option == "--emit-llvm" ||
           option == "--test-entry" || option == "--arithmetic" || option == "--matmul-policy" ||
           option == "--package" || option == "--project") &&
          i + 1 < argc)
        filtered.push_back(argv[++i]);
    }
  }
  if ((!platform.empty() && !toolchainPath.empty()) ||
      (platform.empty() && !platformOptions.empty())) {
    std::cerr << "choose --platform with SDK flags, or --toolchain with its profile\n";
    return 2;
  }
  if (!platform.empty()) {
    const std::map<std::string, std::string> triples{{"linux-x86_64", "x86_64-unknown-linux-gnu"},
                                                     {"linux-arm64", "aarch64-unknown-linux-gnu"},
                                                     {"windows-x86_64", "x86_64-pc-windows-msvc"},
                                                     {"windows-arm64", "aarch64-pc-windows-msvc"},
                                                     {"macos-arm64", "arm64-apple-macosx26.0.0"}};
    auto found = triples.find(platform);
    if (found == triples.end()) {
      std::cerr << "unsupported CPU platform: " << platform << '\n';
      return 2;
    }
    platformOptions["format"] = 1;
    platformOptions["target"] = found->second;
  }
  const bool selectedCpuToolchain = !toolchainPath.empty() || !platform.empty();
  if (target == "cuda,vulkan" || target == "vulkan,cuda") {
    if (selectedCpuToolchain) {
      std::cerr << "GPU bundles require a native host target\n";
      return 2;
    }
#if defined(KALANDRA_HAS_CUDA_SOURCE) && defined(KALANDRA_HAS_VULKAN_SOURCE)
    return kalandra::cli::compileGpuBundle(filtered, feedback);
#else
    std::cerr << "GPU bundles require a compiler with both CUDA and Vulkan enabled\n";
    return 2;
#endif
  }
  if ((target == "metal" || target == "vulkan" || target == "cuda") && selectedCpuToolchain) {
    std::cerr << "--toolchain selects a CPU ABI and cannot be combined with --target " << target
              << '\n';
    return 2;
  }
  if (target == "cuda") {
#ifdef KALANDRA_HAS_CUDA_SOURCE
    char build[] = "--build";
    if (std::none_of(filtered.begin() + 1, filtered.end(),
                     [](const char *argument) { return std::string_view(argument) == "--build"; }))
      filtered.push_back(build);
    return kalandra::compileCudaCommand(static_cast<int>(filtered.size()), filtered.data(), true, feedback);
#else
    std::cerr << "CUDA source support is disabled; configure KALANDRA_BUILD_CUDA_SOURCE and "
                 "KALANDRA_BUILD_CUDA_RUNTIME in a normal Linux compiler build\n";
    return 2;
#endif
  }
  if (target == "vulkan") {
#ifdef KALANDRA_HAS_VULKAN_SOURCE
    char build[] = "--build";
    if (std::none_of(filtered.begin() + 1, filtered.end(),
                     [](const char *argument) { return std::string_view(argument) == "--build"; }))
      filtered.push_back(build);
    return kalandra::compileVulkanCommand(static_cast<int>(filtered.size()), filtered.data(), true, feedback);
#else
    std::cerr << "Vulkan source support is disabled; configure KALANDRA_BUILD_VULKAN_RUNTIME "
                 "in a normal compiler build\n";
    return 2;
#endif
  }
  if (target == "metal") {
#ifdef KALANDRA_HAS_METAL_SOURCE
    char build[] = "--build";
    bool hasBuild = false;
    for (std::size_t i = 1; i < filtered.size(); ++i) {
      std::string_view option = filtered[i];
      if (option == "-o" && i + 1 < filtered.size())
        ++i;
      else if (option == "--build")
        hasBuild = true;
    }
    if (!hasBuild)
      filtered.push_back(build);
    return kalandra::compileMetalCommand(static_cast<int>(filtered.size()), filtered.data(), true, feedback);
#else
#ifdef KALANDRA_INSTALLED_COMPILER
    std::cerr
        << "This installed compiler package supports CPU only; Metal packaging is not included\n";
#else
    std::cerr << "Metal source support is disabled; configure both KALANDRA_BUILD_METAL_RUNTIME "
                 "and KALANDRA_BUILD_METAL_PROOF\n";
#endif
    return 2;
#endif
  }
  if (target != "cpu") {
    std::cerr << "unsupported native target: " << target << '\n';
    return 2;
  }
  const std::span<char *> arguments(filtered);
  if (arguments.size() < 2) {
    std::cerr << usage;
    return 2;
  }
  std::filesystem::path output, mlirPath, llvmPath, headerPath;
  std::optional<std::string> testEntry;
  std::string sourcePath, projectPath;
  std::optional<kalandra::NativeProject> project;
  std::vector<kalandra::ModulePackage> packages;
  std::string emit;
  std::vector<std::string> linkLibraries, systemLinkArgs;
  bool sanitize = false, threadSanitize = false;
  kalandra::ArithmeticMode arithmetic = kalandra::ArithmeticMode::Hardware;
  bool selectedArithmetic = false;
  bool selectedMatmul = false;
  auto optimization = kalandra::Optimization::O0;
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string option = arguments[i];
    if ((option == "-o" || option == "--emit-mlir" || option == "--emit-llvm" ||
         option == "--test-entry" || option == "--package" || option == "--project" || option == "--emit" || option == "--header") &&
        i + 1 < arguments.size()) {
      const std::string value = arguments[++i];
      if (option == "-o")
        output = value;
      else if (option == "--emit") {
        if (!emit.empty() || (value != "c-header" && value != "object" && value != "static" && value != "shared")) {
          std::cerr << "expected --emit object, static, shared or c-header once\n";
          return 2;
        }
        emit = value;
      } else if (option == "--header") {
        if (!headerPath.empty() || value.empty()) return 2;
        headerPath = value;
      } else if (option == "--emit-mlir")
        mlirPath = value;
      else if (option == "--emit-llvm")
        llvmPath = value;
      else if (option == "--test-entry")
        testEntry = value;
      else if (option == "--project") {
        if (value.empty() || !projectPath.empty()) {
          std::cerr << "one nonempty --project is allowed\n";
          return 2;
        }
        projectPath = value;
      } else {
        auto package = kalandra::parseModulePackage(value);
        if (!package) {
          std::cerr << package.error() << '\n';
          return 2;
        }
        packages.push_back(std::move(*package));
      }
    } else if (option == "--arithmetic" && !selectedArithmetic && i + 1 < arguments.size()) {
      auto mode = kalandra::parseArithmeticMode(arguments[++i]);
      if (!mode) {
        std::cerr << "invalid --arithmetic mode\n";
        return 2;
      }
      arithmetic = *mode;
      selectedArithmetic = true;
    } else if (option == "--matmul-policy" && !selectedMatmul && i + 1 < arguments.size()) {
      if (!kalandra::parseMatmulPolicy(arguments[++i])) {
        std::cerr << "invalid --matmul-policy\n";
        return 2;
      }
      // The CPU's existing fixed tree is a valid full-precision implementation
      // under either policy. This option currently selects GPU kernel freedom.
      selectedMatmul = true;
    } else if (option == "--sanitize")
      sanitize = true;
    else if (option == "--thread-sanitize")
      threadSanitize = true;
    else if (auto level = kalandra::parseOptimization(option))
      optimization = *level;
    else if (!option.empty() && !option.starts_with('-') && sourcePath.empty())
      sourcePath = option;
    else {
      std::cerr << "unknown or incomplete option: " << option << '\n';
      return 2;
    }
  }
  kalandra::cli::BuildProgress progress(feedback);
  progress.phase("Checking", projectPath.empty() ? sourcePath : projectPath);
  if (!projectPath.empty()) {
    if (!sourcePath.empty()) {
      progress.diagnostic() << "choose a source file or --project, not both\n";
      return 2;
    }
    auto loaded = kalandra::loadNativeProject(projectPath);
    if (!loaded) {
      progress.diagnostic() << loaded.error() << '\n';
      return 1;
    }
    project = std::move(*loaded);
    sourcePath = project->entry;
    linkLibraries = project->linkLibraries;
    packages.insert(packages.end(), project->packages.begin(), project->packages.end());
  }
  if (sourcePath.empty()) {
    progress.diagnostic() << "missing source file or --project\n";
    return 2;
  }
  const bool library = emit == "object" || emit == "static" || emit == "shared";
  if (project && !project->headers.empty() && (selectedCpuToolchain || !emit.empty())) {
    progress.diagnostic() << "native dependencies currently require a host executable build\n";
    return 2;
  }
  if ((!headerPath.empty() && !library) || (library && testEntry)) {
    std::cerr << "--header requires a library output; --test-entry requires an executable\n";
    return 2;
  }
  if (selectedCpuToolchain && (sanitize || threadSanitize)) {
    progress.diagnostic() << "cross-toolchain sanitizer modes are not yet qualified; use normal or optimized "
                 "builds\n";
    return 2;
  }
  if (sanitize && threadSanitize) {
    progress.diagnostic() << "--sanitize and --thread-sanitize are separate builds\n";
    return 2;
  }
#if defined(_WIN32) && defined(__aarch64__)
  if (sanitize) {
    progress.diagnostic() << "ASan/UBSan are unavailable in the pinned Windows ARM64 toolchain\n";
    return 2;
  }
#endif
#ifdef _WIN32
  if (threadSanitize) {
    progress.diagnostic() << "ThreadSanitizer is unavailable for the Windows native target\n";
    return 2;
  }
#endif
  if (output.empty()) {
    progress.diagnostic() << "missing -o executable\n";
    return 2;
  }
  std::optional<kalandra::InstalledNativeProject> managed;
  if (project && !project->dependencies.empty()) {
    auto loaded = kalandra::loadInstalledNativeProject(*project);
    if (!loaded) {
      progress.diagnostic() << loaded.error() << '\n';
      return 1;
    }
    managed = std::move(*loaded);
    managed->nodes[0].localPackages = packages;
  }
  static int executableAnchor;
  std::error_code resourceError;
  const auto executable = std::filesystem::canonical(
      llvm::sys::fs::getMainExecutable(argv[0], &executableAnchor), resourceError);
  if (resourceError) {
    progress.diagnostic() << "cannot locate compiler executable\n";
    return 1;
  }
  const auto hostPrefix = executable.parent_path().parent_path();
  std::optional<kalandra::CrossToolchain> cross;
  // Installed compilers resolve only their adjacent private runtime resources.
  // Never fall back to the build-tree archives when an installed file is missing.
  std::string clang = KALANDRA_CLANGXX;
  std::optional<std::filesystem::path> clangResources;
#ifdef KALANDRA_INSTALLED_COMPILER
  const auto runtimeRoot = executable.parent_path() / KALANDRA_RUNTIME_RELATIVE_DIR;
  const auto runtimePath = [&](std::string_view name) {
    return cross ? cross->runtimeArchive(name) : (runtimeRoot / name).string();
  };
  const auto toolchainRoot = runtimeRoot.parent_path() / "toolchain";
  if (std::filesystem::exists(toolchainRoot)) {
    clang = (executable.parent_path() / "../libexec/kalandra/clang++"
#ifdef _WIN32
                                        ".exe"
#endif
             )
                .string();
    clangResources = toolchainRoot / "clang";
    if (!std::filesystem::is_regular_file(clang) ||
        !std::filesystem::is_directory(*clangResources / "include") ||
        !std::filesystem::is_directory(*clangResources / "lib")) {
      progress.diagnostic() << "bundled Clang or its compiler resources are missing\n";
      return 1;
    }
  }
#else
  const auto runtimePath = [&](std::string_view name) {
    return cross ? cross->runtimeArchive(name) : std::string(name);
  };
#endif
  if (selectedCpuToolchain) {
    auto loaded = kalandra::loadCrossToolchain(
        progress, platform.empty() ? std::filesystem::path(toolchainPath)
                         : std::filesystem::current_path() / "command-line-toolchain.json",
        clang, hostPrefix,
        platform.empty() ? std::optional<llvm::json::Object>{}
                         : std::optional<llvm::json::Object>(std::move(platformOptions)));
    if (!loaded) {
      progress.diagnostic() << "CPU toolchain: " << loaded.error() << '\n';
      return 2;
    }
    cross = std::move(*loaded);
  }
  const llvm::Triple libraryTarget(cross ? cross->target.triple : llvm::sys::getDefaultTargetTriple());
  std::filesystem::path importLibrary;
  if (emit == "shared" && libraryTarget.isOSWindows()) {
    importLibrary = output;
    importLibrary.replace_extension(".lib");
  }
  std::vector<std::filesystem::path> paths{sourcePath};
  paths.insert(paths.end(), linkLibraries.begin(), linkLibraries.end());
  if (project) {
    for (const auto &import : project->headers)
      if (!import.header.empty())
        paths.emplace_back(import.header);
    paths.emplace_back(project->manifest);
    if (project->libraryEntry)
      paths.emplace_back(*project->libraryEntry);
  }
  for (const auto &path : {output, mlirPath, llvmPath, headerPath, importLibrary}) {
    if (path.empty())
      continue;
    if (project) {
      auto allowed = kalandra::protectNativePackageOutput(*project, path.string());
      if (!allowed) {
        progress.diagnostic() << allowed.error() << '\n';
        return 2;
      }
    }
    for (const auto &previous : paths) {
      std::error_code error;
      const bool equivalent = std::filesystem::equivalent(path, previous, error);
      std::error_code firstError, secondError;
      const auto a = std::filesystem::weakly_canonical(path, firstError);
      const auto b = std::filesystem::weakly_canonical(previous, secondError);
      if ((!error && equivalent) || (!firstError && !secondError && a == b)) {
        progress.diagnostic() << "source and output paths must be distinct\n";
        return 2;
      }
    }
    paths.push_back(path);
  }
  auto syntax = kalandra::loadApplicationModules(
      sourcePath, packages,
      managed ? std::span<const kalandra::PackageNode>(managed->nodes)
              : std::span<const kalandra::PackageNode>{},
      {},
      project ? std::span<const kalandra::HeaderImport>(project->headers)
              : std::span<const kalandra::HeaderImport>{},
      clang, clangResources, &systemLinkArgs);
  if (!syntax) {
    progress.diagnostic() << sourcePath << ": " << syntax.error() << '\n';
    return 1;
  }
  // Every dependency is source input: output publication must not overwrite it,
  // including aliases through symlinks or hard links.
  for (const auto &part : syntax->module.source->parts())
    for (const auto &path : {output, mlirPath, llvmPath, headerPath, importLibrary}) {
      if (path.empty())
        continue;
      std::error_code error, firstError, secondError;
      const bool equivalent = std::filesystem::equivalent(path, part.source->path(), error);
      const auto a = std::filesystem::weakly_canonical(path, firstError);
      const auto b = std::filesystem::weakly_canonical(part.source->path(), secondError);
      if ((!error && equivalent) || (!firstError && !secondError && a == b)) {
        progress.diagnostic() << "module source and output paths must be distinct\n";
        return 2;
      }
    }
  auto typed = kalandra::check(std::move(*syntax));
  if (!typed.ok()) {
    for (const auto &d : typed.diagnostics)
      progress.diagnostic() << kalandra::renderDiagnostic(*typed.source, d);
    return 1;
  }
  const bool publicInterface = library || emit == "c-header";
  if (!testEntry && !publicInterface)
    if (auto error = kalandra::prepareApplication(*typed.module)) {
      progress.diagnostic() << kalandra::renderDiagnostic(*typed.source, *error);
      return 1;
    }
  progress.phase("Compiling", sourcePath);
  auto lowered = publicInterface ? kalandra::lowerCLibrary(*typed.module, cross ? cross->target : kalandra::CpuTarget{}, optimization)
                         : kalandra::lowerNative(*typed.module, testEntry, {}, arithmetic, false,
                                       cross ? cross->target : kalandra::CpuTarget{}, optimization);
  if (!lowered) {
    progress.diagnostic() << "native lowering: " << lowered.error() << '\n';
    return 1;
  }
  if (emit == "c-header") {
    if (testEntry || !mlirPath.empty() || !llvmPath.empty()) {
      std::cerr << "C header output cannot be combined with intermediate outputs or a test entry\n";
      return 2;
    }
    auto header = kalandra::cli::cHeader(*typed.module);
    if (!header) {
      progress.diagnostic() << header.error() << '\n';
      return 1;
    }
    llvm::SmallString<256> stage;
    if (auto error = llvm::sys::fs::createUniqueDirectory(
            std::filesystem::absolute(output.parent_path() / ".kalandra-header").string(), stage)) {
      progress.diagnostic() << "cannot stage C header: " << error.message() << '\n';
      return 1;
    }
    LinkDirectory temporary{stage.str().str()};
    const auto file = temporary.path / "bindings.h";
    if (!write(file, *header) || kalandra::detail::replaceFile(file, output)) {
      progress.diagnostic() << "cannot publish C header\n";
      return 1;
    }
    return 0;
  }
  if (sanitize || threadSanitize) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(lowered->llvm, diagnostic, context);
    if (!module) {
      progress.diagnostic() << "cannot prepare LLVM sanitizer attributes\n";
      return 1;
    }
    for (auto &function : *module)
      if (!function.isDeclaration())
        function.addFnAttr(threadSanitize ? llvm::Attribute::SanitizeThread
                                          : llvm::Attribute::SanitizeAddress);
    if (threadSanitize)
      kalandra::padThreadSanitizerFrames(*module);
    if (invalidModule(*module, progress))
      return 1;
    lowered->llvm.clear();
    llvm::raw_string_ostream outputIR(lowered->llvm);
    module->print(outputIR, nullptr);
  }
  std::vector<std::string> archives{runtimePath(threadSanitize ? KALANDRA_THREAD_SANITIZED_RUNTIME
                                                : sanitize     ? KALANDRA_SANITIZED_RUNTIME
                                                               : KALANDRA_RUNTIME)};
  const auto main =
      std::ranges::find_if(typed.module->functions, [](const auto &f) { return f.name == "main"; });
  const bool scalarMain = main != typed.module->functions.end() && !main->resultType.tensor;
  if (main != typed.module->functions.end() && kalandra::isStringArray(main->resultType))
    archives.insert(archives.begin(),
                    threadSanitize ? runtimePath(KALANDRA_STRING_ARRAY_THREAD_SANITIZED_ENTRY)
                    : sanitize     ? runtimePath(KALANDRA_STRING_ARRAY_SANITIZED_ENTRY)
                                   : runtimePath(KALANDRA_STRING_ARRAY_ENTRY));
  else if (main != typed.module->functions.end() && main->resultType.tensor)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_COLLECTION_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_COLLECTION_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_COLLECTION_ENTRY));
  if (typed.module->applicationEntry)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_APPLICATION_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_APPLICATION_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_APPLICATION_ENTRY));
  if (!typed.module->applicationEntry && scalarMain && main->resultType.element == kalandra::NumericType::Int32)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_INT32_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_INT32_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_INT32_ENTRY));
  if (scalarMain && main->resultType.element == kalandra::NumericType::UInt32)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_UINT32_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_UINT32_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_UINT32_ENTRY));
  if (scalarMain && (main->resultType.element == kalandra::NumericType::Int64 ||
                     main->resultType.element == kalandra::NumericType::Int8 ||
                     main->resultType.element == kalandra::NumericType::Int16))
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_INT64_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_INT64_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_INT64_ENTRY));
  if (scalarMain && main->resultType.element == kalandra::NumericType::Float64)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_FLOAT64_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_FLOAT64_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_FLOAT64_ENTRY));
  if (scalarMain && (main->resultType.element == kalandra::NumericType::UInt64 ||
                     main->resultType.element == kalandra::NumericType::UInt8 ||
                     main->resultType.element == kalandra::NumericType::UInt16))
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_UINT64_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_UINT64_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_UINT64_ENTRY));
  if (scalarMain && main->resultType.builtin == kalandra::BuiltinValue::String)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_STRING_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_STRING_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_STRING_ENTRY));
  if (scalarMain && main->resultType.builtin == kalandra::BuiltinValue::Bool)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_BOOL_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_BOOL_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_BOOL_ENTRY));
  if (scalarMain && main->resultType.builtin == kalandra::BuiltinValue::Unit)
    archives.insert(archives.begin(), threadSanitize
                                          ? runtimePath(KALANDRA_UNIT_THREAD_SANITIZED_ENTRY)
                                      : sanitize ? runtimePath(KALANDRA_UNIT_SANITIZED_ENTRY)
                                                 : runtimePath(KALANDRA_UNIT_ENTRY));
  if (cross && cross->windows()) {
    auto builtins = cross->windowsBuiltins();
    if (!builtins) {
      progress.diagnostic() << "CPU toolchain: " << builtins.error() << '\n';
      return 2;
    }
    archives.push_back(builtins->string());
  }
  if (library)
    archives.clear();
  for (const auto &archive : archives) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(archive, error)) {
      progress.diagnostic() << "native runtime archive is missing: " << archive << '\n';
      return 1;
    }
  }
  if (cross)
    for (const auto &archive : archives)
      if (auto error = cross->verifyArchive(archive)) {
        progress.diagnostic() << "CPU toolchain: " << *error << '\n';
        return 2;
      }
  if (!mlirPath.empty() && !write(mlirPath, lowered->mlir)) {
    progress.diagnostic() << "cannot write MLIR\n";
    return 1;
  }
  if (!llvmPath.empty() && !write(llvmPath, lowered->llvm)) {
    progress.diagnostic() << "cannot write LLVM IR\n";
    return 1;
  }
  // Standalone execution is ordinary. Keep the checked emitted library entry
  // unchanged; only the executable link copy may acquire this context-free main.
  std::string linkerIR = lowered->llvm;
  if (!library) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(linkerIR, diagnostic, context);
    if (!module)
      return 1;
    if (auto *ordinaryMain = kalandra::addOrdinaryMain(*module, typed.module->applicationEntry)) {
      if (sanitize || threadSanitize)
        ordinaryMain->addFnAttr(threadSanitize ? llvm::Attribute::SanitizeThread
                                               : llvm::Attribute::SanitizeAddress);
      if (invalidModule(*module, progress))
        return 1;
      linkerIR.clear();
      llvm::raw_string_ostream text(linkerIR);
      module->print(text, nullptr);
    }
  }
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  if (auto error = llvm::sys::fs::createTemporaryFile("kalandra", "ll", descriptor, temporary)) {
    progress.diagnostic() << "cannot create linker input: " << error.message() << '\n';
    return 1;
  }
  llvm::FileRemover removeIR(temporary);
  {
    llvm::raw_fd_ostream file(descriptor, true);
    file << linkerIR;
    file.close();
    if (file.has_error()) {
      progress.diagnostic() << "cannot write linker input\n";
      return 1;
    }
  }
  if (output.filename().empty() || output.filename() == "." || output.filename() == "..") {
    progress.diagnostic() << "output must name an executable file\n";
    return 1;
  }
  // Mach-O linkers use the output basename as the ad-hoc signing identity.
  // Keep that name stable while staging atomically on the destination volume.
  llvm::SmallString<256> linkedDirectory;
  if (auto error = llvm::sys::fs::createUniqueDirectory(
          std::filesystem::absolute(output.parent_path() / ".kalandra-link").string(),
          linkedDirectory)) {
    progress.diagnostic() << "cannot create output: " << error.message() << '\n';
    return 1;
  }
  LinkDirectory staged{std::string(linkedDirectory)};
  // LLD's Mach-O signing code recognizes '/' on every build host.
  // The caller controls the basename. Isolate the published binary from the
  // archive member, import library and generated header under staging.
  std::error_code directoryError;
  const auto binaryDir = staged.path / "output";
  std::filesystem::create_directory(binaryDir, directoryError);
  if (directoryError) {
    progress.diagnostic() << "cannot stage native output: " << directoryError.message() << '\n';
    return 1;
  }
  const auto linkedOutput = (binaryDir / output.filename()).generic_string();
  std::vector<std::string> command{clang, kalandra::optimizationFlag(optimization),
                                   "-ffp-contract=off", "-Wno-override-module",
                                   std::string(temporary)};
  if (clangResources && !cross)
    command.push_back("-resource-dir=" + clangResources->string());
  if (cross) {
    auto flags = cross->clangArguments();
    command.insert(command.end(), flags.begin(), flags.end());
    if (typed.module->applicationEntry && cross->windows())
      command.push_back("-Wl,/entry:wmainCRTStartup");
  } else {
#ifdef _WIN32
    command.push_back("-fms-runtime-lib=static");
    command.push_back("--rtlib=compiler-rt");
    // The private runtime also contains the numerical main adapter. Select the
    // Unicode CRT entry explicitly so COFF does not extract that archive member
    // merely while choosing between main and wmain.
    if (typed.module->applicationEntry)
      command.push_back("-Wl,/entry:wmainCRTStartup");
#else
    command.push_back("-pthread");
#ifdef __APPLE__
    command.push_back("-mmacosx-version-min=" KALANDRA_NATIVE_MACOS_MINIMUM);
#endif
#endif
  }
  if (library) {
    if (emit == "shared") {
      command.emplace_back("-shared");
      if (libraryTarget.isOSDarwin()) {
        command.insert(command.end(), {"-Xlinker", "-install_name", "-Xlinker",
                                       "@rpath/" + output.filename().string()});
      } else if (libraryTarget.isOSWindows()) {
        command.insert(command.end(), {"-Xlinker", "/implib:" + (staged.path / "import.lib").string()});
      } else if (libraryTarget.isOSLinux()) {
        command.insert(command.end(), {"-Xlinker", "-soname", "-Xlinker", output.filename().string()});
      }
    } else
      command.emplace_back("-c");
#ifndef _WIN32
    if (!cross || !cross->windows())
      command.emplace_back("-fPIC");
#endif
  }
  command.insert(command.end(), archives.begin(), archives.end());
  command.insert(command.end(), linkLibraries.begin(), linkLibraries.end());
  command.insert(command.end(), systemLinkArgs.begin(), systemLinkArgs.end());
  const auto objectPath = (staged.path / "c-exports.o").generic_string();
  command.insert(command.end(), {"-o", emit == "static" ? objectPath : linkedOutput});
  if (sanitize) {
    command.push_back("-fsanitize=address,undefined");
    command.push_back("-fno-sanitize-recover=all");
  }
  if (threadSanitize)
    command.push_back("-fsanitize=thread");
  std::vector<llvm::StringRef> invocation(command.begin(), command.end());
  std::string error;
  auto environment = cross ? kalandra::crossToolEnvironment() : std::vector<std::string>{};
  std::vector<llvm::StringRef> env(environment.begin(), environment.end());
  progress.phase("Linking", output.string());
  const auto status = kalandra::cli::runBuildTool(
      progress, command[0], invocation,
      cross ? std::optional<llvm::ArrayRef<llvm::StringRef>>(env) : std::nullopt, {}, 0, error);
  if (status != 0) {
    progress.diagnostic() << "native link failed: " << error << '\n';
    return 1;
  }
  if (emit == "static") {
    auto member = llvm::NewArchiveMember::getFile(objectPath, true);
    if (!member) {
      progress.diagnostic() << llvm::toString(member.takeError()) << '\n';
      return 1;
    }
    member->MemberName = "c-exports.o";
    if (auto error = llvm::writeArchive(linkedOutput, llvm::ArrayRef<llvm::NewArchiveMember>(*member), llvm::SymtabWritingMode::NormalSymtab,
                                        member->detectKindFromObject(), true, false)) {
      progress.diagnostic() << llvm::toString(std::move(error)) << '\n';
      return 1;
    }
  }
  if (!importLibrary.empty()) {
    if (auto error = kalandra::detail::replaceFile(staged.path / "import.lib", importLibrary)) {
      progress.diagnostic() << "cannot publish Windows import library: " << error.message() << '\n';
      return 1;
    }
  }
  if (!headerPath.empty()) {
    auto header = kalandra::cli::cHeader(*typed.module);
    if (!header || !write(staged.path / "public-header.tmp", *header)) {
      progress.diagnostic() << "cannot stage C header\n";
      return 1;
    }
    if (auto error = kalandra::detail::replaceFile(staged.path / "public-header.tmp", headerPath)) {
      progress.diagnostic() << "cannot publish C header: " << error.message() << '\n';
      return 1;
    }
  }
  if (cross && !library)
    if (auto failure = cross->verifyOutput(std::string(linkedOutput))) {
      progress.diagnostic() << "CPU toolchain: " << *failure << '\n';
      return 1;
    }
  const auto publishError =
      kalandra::detail::replaceFile(std::filesystem::path(std::string(linkedOutput)), output);
  if (publishError) {
    progress.diagnostic() << "cannot publish executable: " << publishError.message() << '\n';
    return 1;
  }
  progress.built(output.string(), cross ? "cpu/" + cross->platform : nativeCpuTarget);
  return 0;
}
