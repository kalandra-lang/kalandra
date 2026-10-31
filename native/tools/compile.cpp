#include "../lib/NativeSanitizerFrames.h"
#include "../lib/OrdinaryMain.h"
#include "CrossToolchain.h"
#include "../lib/PlatformFileSystem.h"
#include "kalandra/Backend.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/Version.h"
#ifdef KALANDRA_HAS_METAL_SOURCE
#include "kalandra/MetalSource.h"
#endif
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

namespace {
bool write(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  output.close();
  return bool(output);
}
} // namespace
int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra"))
    return 0;
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
    auto installed = kalandra::installNativeProject(manifest, {}, options);
    if (!installed) {
      std::cerr << installed.error() << '\n';
      return 1;
    }
    std::cout << (update ? "Updated native dependency graph.\n"
                         : "Installed native dependency graph.\n");
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "new") {
    if (argc != 3 || std::string_view(argv[2]) == "--help") {
      std::cerr << "usage: kalandra new directory\n";
      return argc == 3 ? 0 : 2;
    }
    auto created = kalandra::createNativeProject(argv[2]);
    if (!created) {
      std::cerr << created.error() << '\n';
      return 1;
    }
    std::cout << "Created native project at " << argv[2]
              << "\nCheck and build its kalandra.toml with --project.\n";
    return 0;
  }
  std::vector<char *> filtered{argv[0]};
  std::string target = "cpu", toolchainPath;
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
  if (target == "metal" && !toolchainPath.empty()) {
    std::cerr << "--toolchain selects a CPU ABI and cannot be combined with --target metal\n";
    return 2;
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
    return kalandra::compileMetalCommand(static_cast<int>(filtered.size()), filtered.data());
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
    std::cerr
        << "usage: kalandra source.k -o executable [--emit-mlir file] [--emit-llvm "
           "file] [--test-entry function] [--arithmetic "
           "hardware|software|hardware-with-software-fallback] [--sanitize|--thread-sanitize] "
           "[--matmul-policy optimized|fixed-tree] "
           "[-O0|-O2] [--target "
           "cpu|metal] [--toolchain profile.json] [--package name=path.k ...] [--project manifest.toml instead of source]\n"
           "       kalandra new directory\n";
    return 2;
  }
  std::filesystem::path output, mlirPath, llvmPath;
  std::optional<std::string> testEntry;
  std::string sourcePath, projectPath;
  std::optional<kalandra::NativeProject> project;
  std::vector<kalandra::ModulePackage> packages;
  bool sanitize = false, threadSanitize = false;
  kalandra::ArithmeticMode arithmetic = kalandra::ArithmeticMode::Hardware;
  bool selectedArithmetic = false;
  bool selectedMatmul = false;
  std::string optimization = "-O0";
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string option = arguments[i];
    if ((option == "-o" || option == "--emit-mlir" || option == "--emit-llvm" ||
         option == "--test-entry" || option == "--package" || option == "--project") &&
        i + 1 < arguments.size()) {
      const std::string value = arguments[++i];
      if (option == "-o")
        output = value;
      else if (option == "--emit-mlir")
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
    else if (option == "-O0" || option == "-O2")
      optimization = option;
    else if (!option.empty() && !option.starts_with('-') && sourcePath.empty())
      sourcePath = option;
    else {
      std::cerr << "unknown or incomplete option: " << option << '\n';
      return 2;
    }
  }
  if (!projectPath.empty()) {
    if (!sourcePath.empty()) {
      std::cerr << "choose a source file or --project, not both\n";
      return 2;
    }
    auto loaded = kalandra::loadNativeProject(projectPath);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    project = std::move(*loaded);
    sourcePath = project->entry;
    packages.insert(packages.end(), project->packages.begin(), project->packages.end());
  }
  if (sourcePath.empty()) {
    std::cerr << "missing source file or --project\n";
    return 2;
  }
  if (!toolchainPath.empty() && (sanitize || threadSanitize)) {
    std::cerr << "cross-toolchain sanitizer modes are not yet qualified; use normal or optimized builds\n";
    return 2;
  }
  if (sanitize && threadSanitize) {
    std::cerr << "--sanitize and --thread-sanitize are separate builds\n";
    return 2;
  }
#if defined(_WIN32) && defined(__aarch64__)
  if (sanitize) {
    std::cerr << "ASan/UBSan are unavailable in the pinned Windows ARM64 toolchain\n";
    return 2;
  }
#endif
#ifdef _WIN32
  if (threadSanitize) {
    std::cerr << "ThreadSanitizer is unavailable for the Windows native target\n";
    return 2;
  }
#endif
  if (output.empty()) {
    std::cerr << "missing -o executable\n";
    return 2;
  }
  std::optional<kalandra::InstalledNativeProject> managed;
  if (project && !project->dependencies.empty()) {
    auto loaded = kalandra::loadInstalledNativeProject(*project);
    if (!loaded) {
      std::cerr << loaded.error() << '\n';
      return 1;
    }
    managed = std::move(*loaded);
    managed->nodes[0].localPackages = packages;
  }
  std::vector<std::filesystem::path> paths{sourcePath};
  if (project) {
    paths.emplace_back(project->manifest);
    if (project->libraryEntry)
      paths.emplace_back(*project->libraryEntry);
  }
  for (const auto &path : {output, mlirPath, llvmPath}) {
    if (path.empty())
      continue;
    if (project) {
      auto allowed = kalandra::protectNativePackageOutput(*project, path.string());
      if (!allowed) {
        std::cerr << allowed.error() << '\n';
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
        std::cerr << "source and output paths must be distinct\n";
        return 2;
      }
    }
    paths.push_back(path);
  }
  auto syntax = managed ? kalandra::loadPackageModules(managed->nodes)
                        : kalandra::loadModules(sourcePath, {}, {}, packages);
  if (!syntax) {
    std::cerr << sourcePath << ": " << syntax.error() << '\n';
    return 1;
  }
  // Every dependency is source input: output publication must not overwrite it,
  // including aliases through symlinks or hard links.
  for (const auto &part : syntax->module.source->parts())
    for (const auto &path : {output, mlirPath, llvmPath}) {
      if (path.empty())
        continue;
      std::error_code error, firstError, secondError;
      const bool equivalent = std::filesystem::equivalent(path, part.source->path(), error);
      const auto a = std::filesystem::weakly_canonical(path, firstError);
      const auto b = std::filesystem::weakly_canonical(part.source->path(), secondError);
      if ((!error && equivalent) || (!firstError && !secondError && a == b)) {
        std::cerr << "module source and output paths must be distinct\n";
        return 2;
      }
    }
  auto typed = kalandra::check(std::move(*syntax));
  if (!typed.ok()) {
    for (const auto &d : typed.diagnostics)
      std::cerr << kalandra::renderDiagnostic(*typed.source, d);
    return 1;
  }
  std::optional<kalandra::CrossToolchain> cross;
  // Installed compilers resolve only their adjacent private runtime resources.
  // Never fall back to the build-tree archives when an installed file is missing.
  std::string clang = KALANDRA_CLANGXX;
  std::optional<std::filesystem::path> clangResources;
#ifdef KALANDRA_INSTALLED_COMPILER
  static int executableAnchor;
  std::error_code resourceError;
  const auto executable = std::filesystem::canonical(
      llvm::sys::fs::getMainExecutable(argv[0], &executableAnchor), resourceError);
  if (resourceError) {
    std::cerr << "cannot locate installed compiler executable\n";
    return 1;
  }
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
      std::cerr << "bundled Clang or its compiler resources are missing\n";
      return 1;
    }
  }
#else
  const auto runtimePath = [&](std::string_view name) {
    return cross ? cross->runtimeArchive(name) : std::string(name);
  };
#endif
  if (!toolchainPath.empty()) {
    auto loaded = kalandra::loadCrossToolchain(toolchainPath, clang);
    if (!loaded) { std::cerr << "CPU toolchain: " << loaded.error() << '\n'; return 2; }
    cross = std::move(*loaded);
  }
  auto lowered = kalandra::lowerNative(*typed.module, testEntry, {}, arithmetic, false,
      cross ? cross->target : kalandra::CpuTarget{});
  if (!lowered) {
    std::cerr << "native lowering: " << lowered.error() << '\n';
    return 1;
  }
  if (sanitize || threadSanitize) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(lowered->llvm, diagnostic, context);
    if (!module) {
      std::cerr << "cannot prepare LLVM sanitizer attributes\n";
      return 1;
    }
    for (auto &function : *module)
      if (!function.isDeclaration())
        function.addFnAttr(threadSanitize ? llvm::Attribute::SanitizeThread
                                          : llvm::Attribute::SanitizeAddress);
    if (threadSanitize)
      kalandra::padThreadSanitizerFrames(*module);
    if (llvm::verifyModule(*module, &llvm::errs()))
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
  if (scalarMain && main->resultType.element == kalandra::NumericType::Int32)
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
  if (cross && cross->windows()) {
    auto builtins = cross->windowsBuiltins();
    if (!builtins) { std::cerr << "CPU toolchain: " << builtins.error() << '\n'; return 2; }
    archives.push_back(builtins->string());
  }
  for (const auto &archive : archives) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(archive, error)) {
      std::cerr << "native runtime archive is missing: " << archive << '\n';
      return 1;
    }
  }
  if (cross)
    for (const auto &archive : archives)
      if (auto error = cross->verifyArchive(archive)) {
        std::cerr << "CPU toolchain: " << *error << '\n'; return 2;
      }
  if (!mlirPath.empty() && !write(mlirPath, lowered->mlir)) {
    std::cerr << "cannot write MLIR\n";
    return 1;
  }
  if (!llvmPath.empty() && !write(llvmPath, lowered->llvm)) {
    std::cerr << "cannot write LLVM IR\n";
    return 1;
  }
  // Standalone execution is ordinary. Keep the checked emitted library entry
  // unchanged; only the executable link copy may acquire this context-free main.
  std::string linkerIR = lowered->llvm;
  {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(linkerIR, diagnostic, context);
    if (!module)
      return 1;
    if (auto *ordinaryMain = kalandra::addOrdinaryMain(*module)) {
      if (sanitize || threadSanitize)
        ordinaryMain->addFnAttr(threadSanitize ? llvm::Attribute::SanitizeThread
                                               : llvm::Attribute::SanitizeAddress);
      if (llvm::verifyModule(*module, &llvm::errs()))
        return 1;
      linkerIR.clear();
      llvm::raw_string_ostream text(linkerIR);
      module->print(text, nullptr);
    }
  }
  llvm::SmallString<256> temporary;
  int descriptor = -1;
  if (auto error = llvm::sys::fs::createTemporaryFile("kalandra", "ll", descriptor, temporary)) {
    std::cerr << "cannot create linker input: " << error.message() << '\n';
    return 1;
  }
  llvm::FileRemover removeIR(temporary);
  {
    llvm::raw_fd_ostream file(descriptor, true);
    file << linkerIR;
    file.close();
    if (file.has_error()) {
      std::cerr << "cannot write linker input\n";
      return 1;
    }
  }
  llvm::SmallString<256> linkedOutput;
  if (auto error = llvm::sys::fs::createUniqueFile(output.string() + ".tmp-%%%%%%", descriptor,
                                                   linkedOutput)) {
    std::cerr << "cannot create output: " << error.message() << '\n';
    return 1;
  }
  if (auto error = llvm::sys::Process::SafelyCloseFileDescriptor(descriptor)) {
    std::cerr << "cannot close temporary output: " << error.message() << '\n';
    return 1;
  }
  llvm::FileRemover removeOutput(linkedOutput);
  std::vector<std::string> command{clang, optimization, "-ffp-contract=off", "-Wno-override-module",
                                   std::string(temporary)};
  if (clangResources && !cross)
    command.push_back("-resource-dir=" + clangResources->string());
  if (cross) {
    auto flags = cross->clangArguments();
    command.insert(command.end(), flags.begin(), flags.end());
  } else {
#ifdef _WIN32
  command.push_back("-fms-runtime-lib=static");
  command.push_back("--rtlib=compiler-rt");
#else
  command.push_back("-pthread");
#ifdef __APPLE__
  command.push_back("-mmacosx-version-min=" KALANDRA_NATIVE_MACOS_MINIMUM);
#endif
#endif
  }
  command.insert(command.end(), archives.begin(), archives.end());
  command.insert(command.end(), {"-o", std::string(linkedOutput)});
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
  const auto status = llvm::sys::ExecuteAndWait(command[0], invocation,
      cross ? std::optional<llvm::ArrayRef<llvm::StringRef>>(env) : std::nullopt,
      {}, 0, 0, &error);
  if (status != 0) {
    std::cerr << "native link failed: " << error << '\n';
    return 1;
  }
  if (cross)
    if (auto failure = cross->verifyOutput(std::string(linkedOutput))) {
      std::cerr << "CPU toolchain: " << *failure << '\n'; return 1;
    }
  const auto publishError =
      kalandra::detail::replaceFile(std::filesystem::path(std::string(linkedOutput)), output);
  if (publishError) {
    std::cerr << "cannot publish executable: " << publishError.message() << '\n';
    return 1;
  }
  return 0;
}
