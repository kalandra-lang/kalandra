#include "kalandra/HeaderImport.h"
#include "AppleHeaderImport.h"
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <map>
#include <set>
#include <sstream>

namespace kalandra {
namespace {
#ifndef __APPLE__
using Types = std::map<std::string, std::string, std::less<>>;
std::string scalar(llvm::StringRef type, const Types &aliases) {
  type = type.trim();
  type.consume_front("const ");
  if (type == "void")
    return "unit";
  if (type == "int")
    return "i32";
  if (type == "unsigned int")
    return "u32";
  if (type == "long")
    return sizeof(long) == 8 ? "i64" : "i32";
  if (type == "unsigned long")
    return sizeof(long) == 8 ? "u64" : "u32";
  if (type == "long long")
    return "i64";
  if (type == "unsigned long long")
    return "u64";
  if (type == "float")
    return "f32";
  if (type == "double")
    return "f64";
  auto it = aliases.find(type.str());
  return it == aliases.end() ? "" : it->second;
}

std::string declaration(const llvm::json::Object &fn, const Types &aliases) {
  const auto *type = fn.getObject("type");
  const auto *loc = fn.getObject("loc");
  if (!type || !loc || loc->get("includedFrom") || !loc->get("offset") ||
      fn.getString("storageClass") == "static" || fn.getBoolean("variadic").value_or(false) ||
      fn.getBoolean("inline").value_or(false))
    return {};
  const auto name = fn.getString("name").value_or("");
  const auto signature = type->getString("qualType").value_or("");
  const auto split = signature.find(" (");
  if (name.empty() || split == llvm::StringRef::npos || !signature.ends_with(")") ||
      signature.contains("__attribute__") || signature.contains('['))
    return {};
  auto result = scalar(signature.take_front(split), aliases);
  if (result.empty())
    return {};
  std::string params;
  unsigned arity = 0;
  if (const auto *inner = fn.getArray("inner")) {
    for (const auto &member : *inner) {
      const auto *param = member.getAsObject();
      if (!param || param->getString("kind") != "ParmVarDecl" || !param->getObject("type") ||
          param->get("inner"))
        return {};
      const auto &ty = *param->getObject("type");
      auto arg =
          scalar(ty.getString("desugaredQualType").value_or(ty.getString("qualType").value_or("")),
                 aliases);
      if (arg.empty() || arg == "unit")
        return {};
      if (arity)
        params += ", ";
      params += "arg" + std::to_string(arity++) + ": " + arg;
    }
  }
  if (!arity && !signature.ends_with("(void)"))
    return {};
  return "export extern \"C\" fn " + name.str() + "(" + params + ") -> " + result + "\n";
}

std::expected<std::string, std::string>
readHeader(const std::string &header, const std::string &clang,
           const std::optional<std::filesystem::path> &resources,
           const std::set<std::string> &wanted) {
  llvm::SmallString<256> output;
  if (auto ec = llvm::sys::fs::createTemporaryFile("kalandra-header", "json", output))
    return std::unexpected(ec.message());
  llvm::FileRemover remove(output);
  std::vector<llvm::StringRef> args{clang,
                                    "-x",
                                    "c",
                                    "-std=c17",
                                    "-Werror=unknown-attributes",
                                    "-Werror=ignored-attributes",
                                    "-fsyntax-only",
                                    "-Xclang",
                                    "-ast-dump=json"};
  const auto resourcePath = resources ? resources->string() : std::string{};
  if (resources)
    args.insert(args.end(), {"-resource-dir", resourcePath});
  args.insert(args.end(), {"--", header});
  std::array<std::optional<llvm::StringRef>, 3> io{llvm::StringRef{}, output.str(), std::nullopt};
  std::string error;
  if (llvm::sys::ExecuteAndWait(clang, args, std::nullopt, io, 30, 0, &error) != 0)
    return std::unexpected("Clang could not read C header " + header +
                           (error.empty() ? "" : ": " + error));
  uint64_t size;
  if (llvm::sys::fs::file_size(output, size) || size > 64 * 1024 * 1024)
    return std::unexpected("C header syntax tree exceeds the import size limit: " + header);
  auto bytes = llvm::MemoryBuffer::getFile(output);
  if (!bytes)
    return std::unexpected(bytes.getError().message());
  auto parsed = llvm::json::parse((*bytes)->getBuffer());
  if (!parsed)
    return std::unexpected(llvm::toString(parsed.takeError()));
  const auto *root = parsed->getAsObject();
  const auto *nodes = root ? root->getArray("inner") : nullptr;
  if (!nodes)
    return std::unexpected("missing C translation unit in " + header);
  Types aliases;
  std::set<std::string> emitted;
  std::string source;
  for (const auto &node : *nodes) {
    const auto *decl = node.getAsObject();
    if (!decl)
      continue;
    auto kind = decl->getString("kind");
    auto name = decl->getString("name");
    if (!name)
      continue;
    if (kind == "TypedefDecl") {
      if (const auto *ty = decl->getObject("type"))
        aliases[name->str()] = scalar(
            ty->getString("desugaredQualType").value_or(ty->getString("qualType").value_or("")),
            aliases);
    } else if (kind == "FunctionDecl" && wanted.contains(name->str())) {
      auto line = declaration(*decl, aliases);
      if (line.empty())
        return std::unexpected(header + ": unsupported C prototype for " + name->str());
      if (emitted.insert(name->str()).second)
        source += line;
    }
  }
  for (const auto &name : wanted)
    if (!emitted.contains(name))
      return std::unexpected(header + ": no supported external C scalar prototype for " + name);
  return source;
}
#endif
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
      if (import.system.empty()) {
#ifdef __APPLE__
        return importAppleCHeader(import.header, found->second, *bounds);
#else
        return readHeader(import.header, clang, resources, found->second);
#endif
      }
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
#ifdef __APPLE__
      return importAppleCHeader(*header, found->second, *bounds);
#else
      return readHeader(*header, clang, resources, found->second);
#endif
    }();
    if (!source)
      return std::unexpected(source.error());
    overlays[first + i].header = std::move(bounds);
    overlays[first + i].text = std::move(*source);
  }
  return load();
}
} // namespace kalandra
