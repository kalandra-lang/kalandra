#include "kalandra/HeaderImport.h"
#include "AppleHeaderImport.h"
#include "PortableHeaderImport.h"
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <map>
#include <set>
#include <sstream>

namespace kalandra {
namespace {
// Ask the selected driver for its effective C include search order, including
// the platform SDK and toolchain resource directory.
std::expected<std::string, std::string>
systemHeader(const std::string &name, const std::string &clang,
             const std::optional<std::filesystem::path> &resources) {
  llvm::SmallString<256> output;
  if (auto ec = llvm::sys::fs::createTemporaryFile("kalandra-includes", "txt", output))
    return std::unexpected(ec.message());
  llvm::FileRemover clean(output);
  std::vector<llvm::StringRef> args{clang, "-E", "-v", "-x", "c"};
  const auto resource = resources ? resources->string() : std::string{};
  if (resources)
    args.insert(args.end(), {"-resource-dir", resource});
  args.push_back("-");
  std::array<std::optional<llvm::StringRef>, 3> io{llvm::StringRef{}, llvm::StringRef{},
                                                   output.str()};
  if (llvm::sys::ExecuteAndWait(clang, args, std::nullopt, io, 30))
    return std::unexpected("cannot discover the system C include paths");
  auto bytes = llvm::MemoryBuffer::getFile(output);
  if (!bytes)
    return std::unexpected(bytes.getError().message());
  std::istringstream lines((*bytes)->getBuffer().str());
  bool searching = false;
  for (std::string line; std::getline(lines, line);) {
    const auto text = llvm::StringRef(line).trim();
    if (text == "#include <...> search starts here:") {
      searching = true;
      continue;
    }
    if (text == "End of search list.")
      break;
    if (!searching || text.empty())
      continue;
    auto path = std::filesystem::path(text.str()) / (name + ".h");
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
      return path.string();
  }
  return std::unexpected("no public header found for system dependency '" + name + "'");
}
} // namespace

std::expected<ParseResult, std::string>
loadApplicationModules(const std::string &entry, std::span<const ModulePackage> packages,
                       std::span<const PackageNode> graph, std::vector<SourceOverlay> overlays,
                       std::span<const HeaderImport> headers, std::string clang,
                       std::optional<std::filesystem::path> resources,
                       std::vector<std::string> *systemLinkArgs) {
  auto load = [&] {
    return graph.empty() ? loadModules(entry, {}, {}, packages, overlays)
                         : loadPackageModules(graph, {}, {}, overlays);
  };
  if (headers.empty())
    return load();
  const auto first = overlays.size();
  for (const auto &header : headers)
    overlays.push_back({header.moduleEntry, "fn placeholder() -> unit:\n  return ()\n"});
  auto preliminary = load();
  if (!preliminary || !preliminary->ok())
    return preliminary;
  const auto &module = preliminary->module;
  std::map<std::string, std::set<std::string>> requests;
  for (const auto &import : module.imports) {
    if (!import.target || *import.target >= module.source->parts().size())
      continue;
    auto &names = requests[module.source->parts()[*import.target].source->path()];
    for (const auto &item : import.names)
      names.emplace(module.source->slice(item.name));
  }
  if (clang.empty()) {
    static int anchor;
    const auto executable = llvm::sys::fs::getMainExecutable(nullptr, &anchor);
    const auto prefix = std::filesystem::path(executable).parent_path().parent_path();
    const auto bundled = prefix / "libexec/kalandra/clang++"
#ifdef _WIN32
                                  ".exe"
#endif
        ;
    if (std::filesystem::is_regular_file(bundled)) {
      clang = bundled.string();
      resources = prefix / "lib/kalandra/toolchain/clang";
    } else {
      clang = KALANDRA_IMPORT_CLANG;
      if (!std::filesystem::is_regular_file(clang)) {
        auto host = llvm::sys::findProgramByName("clang");
        if (!host)
          return std::unexpected("a host Clang installation is required for C header imports");
        clang = *host;
      }
    }
  }
  for (size_t i = 0; i < headers.size(); ++i) {
    const auto &import = headers[i];
    auto found = requests.find(import.moduleEntry);
    if (found == requests.end())
      continue;
    auto bounds = std::make_shared<HeaderBounds>();
    auto source = [&]() -> std::expected<std::string, std::string> {
      auto read = [&](const std::string &path) -> std::expected<std::string, std::string> {
#ifdef __APPLE__
        HeaderBounds appleBounds;
        auto apple = importAppleCHeader(path, found->second, appleBounds);
        if (apple) {
          *bounds = std::move(appleBounds);
          return apple;
        }
        // Preserve Apple's diagnostics for counted_by. Only take the portable
        // route here when its AST independently proves a complete buffer contract.
        auto portable = importPortableCHeader(path, clang, resources, found->second, *bounds);
        if (portable && !bounds->empty())
          return portable;
        if (!portable)
          return std::unexpected(apple.error() + "\nPortable C contract: " + portable.error());
        return apple;
#else
        return importPortableCHeader(path, clang, resources, found->second, *bounds);
#endif
      };
      if (import.system.empty())
        return read(import.header);
#ifdef __APPLE__
      auto framework = importAppleFramework(import.system, found->second, *bounds);
      if (!framework)
        return std::unexpected(framework.error());
      if (*framework) {
        if (systemLinkArgs)
          systemLinkArgs->insert(systemLinkArgs->end(),
                                 {"-Xlinker", "-needed_framework", "-Xlinker", import.system});
        return std::move(**framework);
      }
#endif
      auto header = systemHeader(import.system, clang, resources);
      if (!header)
        return std::unexpected(header.error());
      if (systemLinkArgs)
        systemLinkArgs->push_back("-l" + import.system);
      return read(*header);
    }();
    if (!source)
      return std::unexpected(source.error());
    overlays[first + i].header = std::move(bounds);
    overlays[first + i].text = std::move(*source);
  }
  return load();
}
} // namespace kalandra
