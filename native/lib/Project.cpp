#include "kalandra/Project.h"
#include "kalandra/GitRepository.h"
#include "kalandra/GitRevision.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>
static_assert(TOML_LIB_MAJOR == 3 && TOML_LIB_MINOR == 4 && TOML_LIB_PATCH == 0,
              "native manifests require toml++ 3.4.0 headers");
namespace kalandra {
namespace {
bool packageName(std::string_view name) {
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  return !name.empty() && letter(name.front()) && std::ranges::all_of(name, [&](char c) {
    return letter(c) || (c >= '0' && c <= '9') || c == '-';
  });
}
bool semver(std::string_view version) {
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  const auto numeric = [&](std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, digit);
  };
  const auto identifiers = [&](std::string_view value, bool prerelease) {
    while (true) {
      const auto dot = value.find('.');
      const auto item = value.substr(0, dot);
      if (item.empty() ||
          !std::ranges::all_of(item,
                               [&](char c) {
                                 return digit(c) || (c >= 'a' && c <= 'z') ||
                                        (c >= 'A' && c <= 'Z') || c == '-';
                               }) ||
          (prerelease && numeric(item) && item.size() > 1 && item.front() == '0'))
        return false;
      if (dot == std::string_view::npos)
        return true;
      value.remove_prefix(dot + 1);
    }
  };
  const auto suffix = version.find_first_of("-+");
  auto core = version.substr(0, suffix);
  for (int component = 0; component < 3; ++component) {
    const auto dot = core.find('.');
    const auto item = core.substr(0, dot);
    if (!numeric(item) || (item.size() > 1 && item.front() == '0') ||
        ((component == 2) != (dot == std::string_view::npos)))
      return false;
    if (component != 2)
      core.remove_prefix(dot + 1);
  }
  if (suffix == std::string_view::npos)
    return true;
  auto tail = version.substr(suffix);
  if (tail.front() == '-') {
    tail.remove_prefix(1);
    const auto plus = tail.find('+');
    if (!identifiers(tail.substr(0, plus), true))
      return false;
    if (plus == std::string_view::npos)
      return true;
    tail.remove_prefix(plus);
  }
  return tail.front() == '+' && identifiers(tail.substr(1), false);
}
struct LibraryMetadata {
  std::string name;
  std::string version;
  std::string entry;
};
struct Manifest {
  std::string path;
  std::optional<std::string> project;
  std::optional<LibraryMetadata> library;
  std::vector<ModulePackage> packages;
  std::vector<NativeDependency> dependencies;
  std::string contents;
};
std::expected<Manifest, std::string> loadManifest(std::string path, ProjectLimits limits) {
  namespace fs = std::filesystem;
  if (path.find('\0') != std::string::npos || fs::path(path).extension() != ".toml")
    return std::unexpected("native project manifests must use the .toml extension");
  std::error_code error;
  const auto manifest = fs::canonical(path, error);
  if (error)
    return std::unexpected(path + ": cannot open native project manifest");
  std::ifstream input(manifest, std::ios::binary);
  if (!input)
    return std::unexpected(path + ": cannot read native project manifest");
  std::string text;
  char buffer[4096];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    if (std::size_t(input.gcount()) > limits.maxBytes - text.size())
      return std::unexpected(path + ": native project manifest exceeds configured byte limit");
    text.append(buffer, std::size_t(input.gcount()));
  }
  if (!input.eof())
    return std::unexpected(path + ": cannot read native project manifest");
  try {
    auto table = toml::parse(text, manifest.string());
    const auto fail = [&](const toml::node &node, const std::string &message) {
      const auto &at = node.source().begin;
      return std::unexpected(manifest.string() + ":" + std::to_string(at.line) + ":" +
                             std::to_string(at.column) + ": " + message);
    };
    for (const auto &[key, value] : table)
      if (key != "format" && key != "project" && key != "packages" && key != "package" &&
          key != "dependencies")
        return fail(value, "unknown native project field '" + std::string(key.str()) + "'");
    auto *format = table.get("format");
    if (!format || !format->is_integer() || format->value<std::int64_t>() != 1)
      return fail(format ? *format : table, "native project format must be integer 1");
    const auto resolve = [&](const std::string &name, const std::string &value,
                             bool confined) -> std::optional<std::string> {
      const fs::path requested(value);
      if (!packageName(name) || !parseModulePackage(name + "=" + value) ||
          requested.has_root_path() || value.find('\\') != std::string::npos)
        return {};
      const auto source = manifest.parent_path() / requested;
      if (!confined)
        return source.lexically_normal().string();
      std::error_code error;
      const auto canonical = fs::weakly_canonical(source, error);
      const auto relative = canonical.lexically_relative(manifest.parent_path());
      if (error || relative.empty() || relative.has_root_path() || *relative.begin() == "..")
        return {};
      return canonical.string();
    };
    Manifest result{manifest.string(), {}, {}, {}, {}, text};
    if (const auto *node = table.get("project")) {
      const auto *project = node->as_table();
      if (!project)
        return fail(*node, "native project requires a [project] table");
      for (const auto &[key, value] : *project)
        if (key != "entry")
          return fail(value, "unknown project field '" + std::string(key.str()) + "'");
      auto entry = (*project)["entry"].value<std::string>();
      if (!entry)
        return fail(project->get("entry") ? *project->get("entry") : *project,
                    "project.entry must be a relative .k path");
      result.project = resolve("entry", *entry, false);
      if (!result.project)
        return fail(*project->get("entry"),
                    "project.entry must be a relative .k path using '/' separators");
    }
    if (const auto *node = table.get("package")) {
      const auto *package = node->as_table();
      if (!package)
        return fail(*node, "package must be a [package] table");
      for (const auto &[key, value] : *package)
        if (key != "name" && key != "version" && key != "entry")
          return fail(value, "unknown package field '" + std::string(key.str()) + "'");
      auto name = (*package)["name"].value<std::string>();
      if (!name || !packageName(*name))
        return fail(package->get("name") ? *package->get("name") : *package,
                    "package.name must be an ASCII identifier or hyphenated name");
      auto version = (*package)["version"].value<std::string>();
      if (!version || !semver(*version))
        return fail(package->get("version") ? *package->get("version") : *package,
                    "package.version must be a valid SemVer string");
      auto entry = (*package)["entry"].value<std::string>();
      auto source = entry ? resolve("entry", *entry, true) : std::nullopt;
      if (!source)
        return fail(package->get("entry") ? *package->get("entry") : *package,
                    "package.entry must be a relative .k path within the package root");
      result.library = LibraryMetadata{std::move(*name), std::move(*version), std::move(*source)};
    }
    if (!result.project && !result.library)
      return fail(table, "native manifest requires a [project] or [package] entry");
    if (auto *packages = table.get("packages")) {
      auto *items = packages->as_table();
      if (!items)
        return fail(*packages, "packages must be a table of names and relative .k paths");
      if (items->size() > limits.maxPackages)
        return fail(*items, "native project exceeds configured package limit");
      for (const auto &[key, node] : *items) {
        auto value = node.value<std::string>();
        auto package = value ? resolve(std::string(key.str()), *value, result.library.has_value())
                             : std::nullopt;
        if (!package)
          return fail(node, result.library ? "package local entries require valid names and "
                                             "relative .k paths within the package root"
                                           : "package entries require valid names and relative .k "
                                             "paths using '/' separators");
        result.packages.push_back({std::string(key.str()), std::move(*package)});
      }
    }
    if (const auto *node = table.get("dependencies")) {
      const auto *dependencies = node->as_table();
      if (!dependencies)
        return fail(*node, "dependencies must be a table of Git requests");
      if (dependencies->size() > limits.maxPackages ||
          result.packages.size() > limits.maxPackages - dependencies->size())
        return fail(*node, "native manifest exceeds configured dependency/mapping limit");
      for (const auto &[key, value] : *dependencies) {
        const std::string name(key.str());
        if (!packageName(name) || std::ranges::any_of(result.packages, [&](const auto &local) {
              return local.name == name;
            }))
          return fail(value, "invalid or conflicting dependency alias '" + name + "'");
        const auto *request = value.as_table();
        if (!request)
          return fail(value, "dependency requests require git and rev strings");
        for (const auto &[field, entry] : *request)
          if (field != "git" && field != "rev")
            return fail(entry, "unknown dependency field '" + std::string(field.str()) + "'");
        auto repository = (*request)["git"].value<std::string>();
        if (!repository || !gitTransport(*repository))
          return fail(
              request->get("git") ? *request->get("git") : value,
              "dependency git requires a normalized local path or supported remote URL");
        auto revision = (*request)["rev"].value<std::string>();
        if (!revision || !validGitRevision(*revision))
          return fail(request->get("rev") ? *request->get("rev") : value,
                      "dependency rev requires a full lowercase commit or supported branch/tag name");
        result.dependencies.push_back({name, std::move(*repository), std::move(*revision)});
      }
    }
    return result;
  } catch (const toml::parse_error &error) {
    std::ostringstream message;
    message << error;
    return std::unexpected(message.str());
  }
}
} // namespace
std::expected<NativeProject, std::string> loadNativeProject(std::string path,
                                                            ProjectLimits limits) {
  auto manifest = loadManifest(std::move(path), limits);
  if (!manifest)
    return std::unexpected(manifest.error());
  if (!manifest->project)
    return std::unexpected(manifest->path + ": native project requires a [project] entry");
  return NativeProject{std::move(manifest->path),
                       std::move(*manifest->project),
                       std::move(manifest->packages),
                       manifest->library ? std::optional(manifest->library->entry) : std::nullopt,
                       std::move(manifest->dependencies),
                       std::move(manifest->contents)};
}
std::expected<NativePackage, std::string> loadNativePackage(std::string path,
                                                            ProjectLimits limits) {
  auto manifest = loadManifest(std::move(path), limits);
  if (!manifest)
    return std::unexpected(manifest.error());
  if (!manifest->library)
    return std::unexpected(manifest->path + ": native library requires a [package] entry");
  auto &library = *manifest->library;
  return NativePackage{std::move(manifest->path),        std::move(library.name),
                       std::move(library.version),       std::move(library.entry),
                       std::move(manifest->project),     std::move(manifest->packages),
                       std::move(manifest->dependencies)};
}
PackageNode NativePackage::node(std::string identity,
                                std::vector<PackageDependency> dependencies) const {
  return {std::move(identity), std::filesystem::path(manifest).parent_path().string(), entry,
          std::move(dependencies), packages};
}
} // namespace kalandra
