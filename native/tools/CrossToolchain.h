#pragma once
#include "kalandra/Backend.h"
#include "kalandra/Version.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Process.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include <filesystem>
#include <fstream>
#include <map>

namespace kalandra {
inline std::vector<std::string> crossToolEnvironment() {
  std::vector<std::string> environment;
  for (const auto *key : {"PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "TMPDIR",
                           "HOME", "USERPROFILE", "LANG", "LC_ALL"})
    if (auto value = llvm::sys::Process::GetEnv(key))
      environment.push_back(std::string(key) + "=" + *value);
  // A present, empty LIB prevents Clang's MSVC driver from adding registry-
  // discovered SDK paths before the profile's explicit -L directories.
  environment.emplace_back("LIB=");
  return environment;
}

// Explicit user-selected toolchains are local configuration, never fetched or
// executed implicitly from project manifests or imported packages.
struct CrossToolchain {
  CpuTarget target;
  std::string platform;
  std::filesystem::path sysroot, package, linker, gccDirectory, resources;
  std::vector<std::filesystem::path> libraryDirectories;
  std::map<std::string, std::string> inventory;

  bool windows() const { return platform.starts_with("windows-"); }
  bool darwin() const { return platform == "macos-arm64"; }
  std::string runtimeArchive(std::string_view hostName) const {
    auto name = std::filesystem::path(hostName).filename().string();
    if (name.starts_with("lib")) name.erase(0, 3);
    if (name.ends_with(".lib")) name.resize(name.size() - 4);
    else if (name.ends_with(".a")) name.resize(name.size() - 2);
    return (package / "lib/kalandra/native" /
        (windows() ? name + ".lib" : "lib" + name + ".a")).string();
  }
  bool matchesObject(const llvm::object::ObjectFile &object) const {
    if (object.getArch() != llvm::Triple(target.triple).getArch() || object.getBytesInAddress() != 8)
      return false;
    if (windows()) {
      const auto *coff = llvm::dyn_cast<llvm::object::COFFObjectFile>(&object);
      return coff && coff->getMachine() == (llvm::Triple(target.triple).isAArch64() ? 0xaa64 : 0x8664);
    }
    return darwin() ? object.isMachO() : object.isELF();
  }
  std::optional<std::string> verifyPackageBytes(const std::filesystem::path &path,
                                               llvm::StringRef bytes) const {
    auto relative = path.lexically_relative(package).generic_string();
    auto found = inventory.find(relative);
    if (found == inventory.end()) return "target runtime is absent from package inventory: " + relative;
    const auto digest = llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
    if (llvm::toHex(digest, true) != found->second)
      return "target runtime checksum mismatch: " + relative;
    return {};
  }
  std::optional<std::string> verifyArchive(const std::filesystem::path &path) const {
    const auto relative = path.lexically_relative(package).generic_string();
    auto buffer = llvm::MemoryBuffer::getFile(path.string());
    if (!buffer) return "cannot read target runtime archive: " + path.string();
    if (auto error = verifyPackageBytes(path, (*buffer)->getBuffer())) return error;
    auto archive = llvm::object::Archive::create((*buffer)->getMemBufferRef());
    if (!archive) return llvm::toString(archive.takeError());
    if ((*archive)->isThin()) return "target runtime must be a self-contained archive";
    llvm::Error error = llvm::Error::success();
    unsigned objects = 0;
    for (auto child : (*archive)->children(error)) {
      auto contents = child.getMemoryBufferRef();
      if (!contents) return llvm::toString(contents.takeError());
      auto object = llvm::object::ObjectFile::createObjectFile(*contents);
      if (!object) return llvm::toString(object.takeError());
      if (!matchesObject(**object))
        return "target runtime object ABI mismatch: " + relative;
      ++objects;
    }
    if (error) return llvm::toString(std::move(error));
    if (!objects) return "target runtime archive contains no objects";
    return {};
  }
  std::optional<std::string> verifyOutput(const std::filesystem::path &path) const {
    auto object = llvm::object::ObjectFile::createObjectFile(path.string());
    if (!object) return llvm::toString(object.takeError());
    auto *value = object->getBinary();
    if (!matchesObject(*value))
      return "linked executable does not match the destination ABI";
    return {};
  }
  std::expected<std::filesystem::path, std::string> windowsBuiltins() const {
    std::vector<std::filesystem::path> candidates;
    for (const auto &[name, hash] : inventory) {
      (void)hash;
      if (name.starts_with("lib/kalandra/toolchain/clang/lib/") &&
          name.find("clang_rt.builtins") != std::string::npos && name.ends_with(".lib")) {
        const auto path = package / name;
        if (!verifyArchive(path)) candidates.push_back(path);
      }
    }
    if (candidates.size() != 1)
      return std::unexpected("target package needs one checksum-verified Windows compiler-rt builtins archive");
    return candidates.front();
  }
  std::vector<std::string> clangArguments() const {
    std::vector<std::string> args{"--no-default-config", "--target=" + target.triple,
        "--sysroot=" + sysroot.string(),
        "-resource-dir=" + resources.string()};
    if (!gccDirectory.empty()) args.push_back("--gcc-install-dir=" + gccDirectory.string());
    for (const auto &directory : libraryDirectories) args.push_back("-L" + directory.string());
    if (windows()) {
      // MSVC's driver ignores --ld-path. Its program lookup also prefixes
      // absolute -fuse-ld values with the target triple; a private -B directory
      // plus an explicit relative basename selects the configured executable
      // directly, bypassing its special link/lld aliases too.
      args.insert(args.end(), {"-B" + linker.parent_path().string(),
                               "-fuse-ld=./" + linker.filename().string()});
      // Never inherit the host's Visual Studio installation or LIB paths.
      args.insert(args.end(), {"-Wno-msvc-not-found", "-nostdlib", "-fms-runtime-lib=static",
          "-Xlinker", "/defaultlib:libcmt", "-Xlinker", "/defaultlib:oldnames",
          "-Xlinker", "/defaultlib:kernel32", "-Xlinker", "/Brepro"});
    } else {
      args.push_back("--ld-path=" + linker.string());
      args.push_back("-pthread");
      if (darwin()) {
        // --ld-path chooses a binary but does not tell Clang its flavor.
        // Non-Darwin hosts otherwise send legacy Apple ld flags to LLD.
        args.insert(args.end(), {"-fuse-ld=lld", "-mmacosx-version-min=" +
            llvm::Triple(target.triple).getOSVersion().getAsString()});
      }
      else if (llvm::Triple(target.triple).isAArch64()) args.push_back("-Wl,-z,max-page-size=16384");
    }
    return args;
  }
};

inline std::expected<CrossToolchain, std::string>
loadCrossToolchain(const std::filesystem::path &profile, const std::string &clang) {
  auto readJson = [](const std::filesystem::path &path) -> std::expected<llvm::json::Value, std::string> {
    auto buffer = llvm::MemoryBuffer::getFile(path.string());
    if (!buffer) return std::unexpected("cannot read toolchain configuration: " + path.string());
    auto parsed = llvm::json::parse((*buffer)->getBuffer());
    if (!parsed) return std::unexpected(llvm::toString(parsed.takeError()));
    return std::move(*parsed);
  };
  auto config = readJson(profile);
  if (!config) return std::unexpected(config.error());
  auto *object = config->getAsObject();
  if (!object || object->getInteger("format") != 1)
    return std::unexpected("CPU toolchain requires JSON format 1");
  for (const auto &[key, value] : *object) {
    (void)value;
    if (key != "format" && key != "target" && key != "sysroot" && key != "runtimePackage" &&
        key != "linker" && key != "gccDirectory" && key != "libraryDirectories")
      return std::unexpected("unknown CPU toolchain field: " + key.str());
  }
  auto requested = object->getString("target");
  if (!requested || requested->contains('\0')) return std::unexpected("CPU toolchain requires a target triple");
  CrossToolchain result;
  result.target.triple = llvm::Triple::normalize(*requested);
  const llvm::Triple triple(result.target.triple);
  if (triple.getArchName() != "x86_64" && triple.getArchName() != "aarch64" && triple.getArchName() != "arm64")
    return std::unexpected("unsupported CPU toolchain architecture");
  const auto arch = triple.isAArch64() ? "arm64" : "x86_64";
  if (triple.isOSLinux() && triple.getEnvironment() == llvm::Triple::GNU)
    result.platform = std::string("linux-") + arch;
  else if (triple.isOSWindows() && triple.getEnvironment() == llvm::Triple::MSVC)
    result.platform = std::string("windows-") + arch;
  else if (triple.getOS() == llvm::Triple::MacOSX && triple.isAArch64() && triple.getOSVersion().getMajor() >= 26)
    result.platform = "macos-arm64";
  else return std::unexpected("unsupported CPU toolchain OS/ABI or macOS minimum");
  auto path = [&](llvm::StringRef key, bool file = false) -> std::expected<std::filesystem::path, std::string> {
    auto text = object->getString(key);
    const bool bundledImports = key == "sysroot" && result.darwin() && !object->get(key);
    if (!bundledImports && (!text || text->empty() || text->contains('\0')))
      return std::unexpected("CPU toolchain requires path: " + key.str());
    std::error_code error;
    auto value = bundledImports
        ? result.package / "share/kalandra-native-tools/macos-link-imports/interfaces"
        : std::filesystem::path(text->str());
    if (value.is_relative()) value = std::filesystem::absolute(profile).parent_path() / value;
    const auto checked = std::filesystem::canonical(value, error);
    if (error || (file ? !std::filesystem::is_regular_file(checked) : !std::filesystem::is_directory(checked)))
      return std::unexpected("invalid CPU toolchain path: " + key.str());
    // LLVM's ld.lld/ld64.lld/lld-link aliases select their driver by argv[0].
    // Validate the resolved file while retaining that executable basename.
    if (file) {
      const auto parent = std::filesystem::canonical(value.parent_path(), error);
      if (error) return std::unexpected("invalid linker parent directory");
      return parent / value.filename();
    }
    return checked;
  };
  for (auto [key, destination] : {
      std::pair{"runtimePackage", &result.package}, {"linker", &result.linker}, {"sysroot", &result.sysroot}}) {
    auto value = path(key, std::string_view(key) == "linker");
    if (!value) return std::unexpected(value.error());
    *destination = *value;
  }
  if (result.platform.starts_with("linux-")) {
    auto gcc = path("gccDirectory");
    if (!gcc) return std::unexpected(gcc.error());
    result.gccDirectory = *gcc;
    for (const auto &name : {"crtbeginS.o", "libgcc.a", "libstdc++.so"})
      if (!std::filesystem::exists(result.gccDirectory / name))
        return std::unexpected("target GCC directory lacks " + std::string(name));
  } else if (object->get("gccDirectory")) return std::unexpected("gccDirectory requires a Linux target");
  if (auto *directories = object->getArray("libraryDirectories")) {
    for (const auto &value : *directories) {
      auto text = value.getAsString();
      if (!text || text->empty() || text->contains('\0'))
        return std::unexpected("invalid target library directory");
      std::filesystem::path directory(text->str());
      if (directory.is_relative()) directory = std::filesystem::absolute(profile).parent_path() / directory;
      std::error_code error;
      directory = std::filesystem::canonical(directory, error);
      if (error || !std::filesystem::is_directory(directory))
        return std::unexpected("target library directory is missing");
      result.libraryDirectories.push_back(directory);
    }
  } else if (object->get("libraryDirectories")) return std::unexpected("libraryDirectories must be an array");
  if (result.windows() && result.libraryDirectories.empty())
    return std::unexpected("Windows target requires explicit SDK/CRT library directories");
  auto metadata = readJson(result.package / "share/kalandra-native-tools/toolchain.json");
  if (!metadata) return std::unexpected(metadata.error());
  auto *info = metadata->getAsObject();
  if (!info || info->getString("platform") != result.platform || info->getString("version") != llvm::StringRef(version.data(), version.size()) ||
      info->getString("llvmVersion") != LLVM_VERSION_STRING)
    return std::unexpected("target runtime package platform/version/LLVM mismatch");
  if (result.darwin()) {
    auto minimum = info->getString("minimumMacOS");
    llvm::VersionTuple runtimeMinimum;
    if (!minimum || runtimeMinimum.tryParse(*minimum) || runtimeMinimum > triple.getOSVersion())
      return std::unexpected("target runtime package has an incompatible macOS minimum");
  }
  result.resources = result.package / "lib/kalandra/toolchain/clang";
  if (!std::filesystem::is_directory(result.resources / "include") ||
      !std::filesystem::is_directory(result.resources / "lib"))
    return std::unexpected("target package lacks standalone compiler resources");
  std::ifstream sums(result.package / "SHA256SUMS");
  std::string line;
  while (std::getline(sums, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 67 || line.substr(64, 2) != "  " ||
        !result.inventory.emplace(line.substr(66), line.substr(0, 64)).second)
      return std::unexpected("invalid target package inventory");
  }
  if (!sums.eof() || result.inventory.empty()) return std::unexpected("target package inventory is missing");
  if (result.darwin() && !object->get("sysroot")) {
    for (const auto *name : {"libSystem.tbd", "libc++.tbd"}) {
      auto file = result.sysroot / "usr/lib" / name;
      auto contents = llvm::MemoryBuffer::getFile(file.string());
      if (!contents) return std::unexpected("bundled macOS import interface is missing");
      if (auto error = result.verifyPackageBytes(file, (*contents)->getBuffer()))
        return std::unexpected(*error);
    }
  }
  // Query the actual pinned host Clang for the destination layout, not the host
  // layout or user-supplied layout text. No target binary is executed.
  llvm::SmallString<256> input, output;
  if (auto error = llvm::sys::fs::createTemporaryFile("kalandra-abi", "c", input))
    return std::unexpected(error.message());
  llvm::FileRemover removeInput(input);
  if (auto error = llvm::sys::fs::createTemporaryFile("kalandra-abi", "ll", output))
    return std::unexpected(error.message());
  llvm::FileRemover removeOutput(output);
  { std::ofstream source(input.c_str()); source << "int kalandra_target_abi;\n"; }
  std::vector<std::string> command{clang, "--no-default-config", "--target=" + result.target.triple, "-S", "-emit-llvm",
      "-x", "c", std::string(input), "-o", std::string(output)};
  if (result.windows()) command.push_back("-Wno-msvc-not-found");
  std::vector<llvm::StringRef> invocation(command.begin(), command.end());
  std::string failure;
  auto environment = crossToolEnvironment();
  std::vector<llvm::StringRef> env(environment.begin(), environment.end());
  if (llvm::sys::ExecuteAndWait(clang, invocation, env, {}, 30, 0, &failure) != 0)
    return std::unexpected("cannot query destination ABI: " + failure);
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyFile(output, diagnostic, context);
  if (!module || module->getTargetTriple().getArch() != triple.getArch() ||
      module->getTargetTriple().getOS() != triple.getOS())
    return std::unexpected("Clang reported an incompatible destination ABI");
  result.target.dataLayout = module->getDataLayoutStr();
  return result;
}
} // namespace kalandra
