#include "kalandra/Syntax.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace kalandra {
namespace {
bool packageName(std::string_view name) {
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (name.empty() || !letter(name.front()))
    return false;
  return std::ranges::all_of(
      name, [&](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '-'; });
}
bool packageEntry(std::string_view entry) {
  return !entry.empty() && entry.find('\0') == std::string_view::npos &&
         entry.find_first_of("\r\n") == std::string_view::npos &&
         std::filesystem::path(entry).extension() == ".k";
}
std::filesystem::path overlayPath(const std::filesystem::path &path, std::error_code &error) {
  auto absolute = std::filesystem::absolute(path, error);
  if (error)
    return {};
  return std::filesystem::weakly_canonical(absolute, error);
}
} // namespace
std::expected<ModulePackage, std::string> parseModulePackage(std::string_view value) {
  const auto equal = value.find('=');
  if (equal == std::string_view::npos || !packageName(value.substr(0, equal)) ||
      !packageEntry(value.substr(equal + 1)))
    return std::unexpected(
        "package mappings require name=path.k with an ASCII identifier or hyphenated name");
  return ModulePackage{std::string(value.substr(0, equal)), std::string(value.substr(equal + 1))};
}
static std::expected<ParseResult, std::string>
loadModuleGraph(std::string path, SyntaxLimits syntaxLimits, ModuleLimits moduleLimits,
                std::span<const ModulePackage> packages, std::span<const SourceOverlay> overlays,
                std::span<const PackageNode> graph) {
  namespace fs = std::filesystem;
  struct File {
    std::shared_ptr<const SourceFile> source;
    std::vector<std::size_t> targets;
    bool complete = false;
    std::size_t height = 0;
  };
  std::vector<File> files;
  std::unordered_map<std::string, std::size_t> known;
  std::size_t bytes = 0;
  std::optional<ParseResult> failure;
  const auto fail = [&](std::shared_ptr<const SourceFile> source, SourceSpan span, std::string code,
                        std::string message) {
    failure = ParseResult{};
    failure->module.source = std::move(source);
    failure->diagnostics.push_back({std::move(code), std::move(message), span});
  };
  auto root = std::make_shared<const SourceFile>(path, "");
  std::unordered_map<std::string, std::shared_ptr<const SourceFile>> replacements;
  std::size_t overlayBytes = 0;
  if (overlays.size() > moduleLimits.maxFiles) {
    fail(root, {}, "M004", "source overlays exceed configured file limit");
    return std::move(*failure);
  }
  for (const auto &overlay : overlays) {
    if (!packageEntry(overlay.path)) {
      fail(root, {}, "M006", "source overlays require nonempty .k paths");
      return std::move(*failure);
    }
    std::error_code error;
    const auto canonical = overlayPath(overlay.path, error);
    if (error || canonical.empty()) {
      fail(root, {}, "M006", "cannot resolve source overlay '" + overlay.path + "'");
      return std::move(*failure);
    }
    if (overlayBytes >= syntaxLimits.maxSourceBytes ||
        overlay.text.size() >= syntaxLimits.maxSourceBytes - overlayBytes) {
      fail(root, {}, "M004", "source overlays exceed configured byte limit");
      return std::move(*failure);
    }
    overlayBytes += overlay.text.size() + 1;
    const auto key = canonical.string();
    if (!replacements.emplace(key, std::make_shared<const SourceFile>(key, overlay.text)).second) {
      fail(root, {}, "M006", "duplicate source overlay for '" + key + "'");
      return std::move(*failure);
    }
  }
  std::unordered_map<std::string, fs::path> entries;
  if (packages.size() > moduleLimits.maxFiles) {
    fail(root, {}, "M004", "package mappings exceed configured file limit");
    return std::move(*failure);
  }
  for (const auto &package : packages) {
    if (!packageName(package.name) || !packageEntry(package.entry) ||
        !entries.emplace(package.name, fs::path(package.entry)).second) {
      fail(root, {}, "M005", "invalid or duplicate local package mapping '" + package.name + "'");
      return std::move(*failure);
    }
  }
  struct Scope {
    fs::path root;
    std::unordered_map<std::string, std::pair<fs::path, std::size_t>> entries;
  };
  if (graph.size() > moduleLimits.maxFiles) {
    fail(root, {}, "M004", "package graph exceeds configured node limit");
    return std::move(*failure);
  }
  std::vector<Scope> scopes(graph.empty() ? 1 : graph.size());
  if (graph.empty()) {
    for (const auto &[name, entry] : entries)
      scopes[0].entries.emplace(name, std::pair{entry, std::size_t(0)});
  } else {
    std::unordered_map<std::string, std::size_t> identities;
    std::size_t edges = 0;
    for (std::size_t owner = 0; owner < graph.size(); ++owner) {
      const auto &node = graph[owner];
      if (node.identity.empty() || !identities.emplace(node.identity, owner).second ||
          !packageEntry(node.entry) || node.root.empty() ||
          node.root.find('\0') != std::string::npos) {
        fail(root, {}, "M007", "invalid or duplicate package identity/root/entry");
        return std::move(*failure);
      }
      std::error_code error;
      scopes[owner].root = fs::canonical(node.root, error);
      if (error || !fs::is_directory(scopes[owner].root, error) || error) {
        fail(root, {}, "M007", "cannot resolve package root '" + node.root + "'");
        return std::move(*failure);
      }
      for (const auto &dependency : node.dependencies) {
        if (++edges > moduleLimits.maxFiles) {
          fail(root, {}, "M004", "package graph exceeds configured edge limit");
          return std::move(*failure);
        }
        if (!packageName(dependency.name) || dependency.target >= graph.size() ||
            dependency.target == 0 ||
            !scopes[owner]
                 .entries
                 .emplace(dependency.name,
                          std::pair{fs::path(graph[dependency.target].entry), dependency.target})
                 .second) {
          fail(root, {}, "M007",
               "invalid or duplicate package dependency '" + dependency.name + "'");
          return std::move(*failure);
        }
      }
      for (const auto &local : node.localPackages) {
        if (++edges > moduleLimits.maxFiles) {
          fail(root, {}, "M004", "package graph exceeds configured edge limit");
          return std::move(*failure);
        }
        if (!packageName(local.name) || !packageEntry(local.entry) ||
            !scopes[owner]
                 .entries.emplace(local.name, std::pair{fs::path(local.entry), owner})
                 .second) {
          fail(root, {}, "M007", "invalid or duplicate package local mapping '" + local.name + "'");
          return std::move(*failure);
        }
      }
    }
    // Validate even unused dependency edges. Cache heights so a shared node
    // cannot conceal a too-deep path through a different caller.
    std::vector<unsigned char> state(graph.size());
    std::vector<std::size_t> heights(graph.size());
    std::function<bool(std::size_t, std::size_t)> validate = [&](std::size_t node,
                                                                 std::size_t depth) {
      if (depth > moduleLimits.maxDepth) {
        fail(root, {}, "M004", "package graph exceeds configured dependency depth");
        return false;
      }
      if (state[node] == 1) {
        fail(root, {}, "M007", "package dependency cycle reaches '" + graph[node].identity + "'");
        return false;
      }
      if (state[node] == 2) {
        if (heights[node] <= moduleLimits.maxDepth - depth)
          return true;
        fail(root, {}, "M004", "package graph exceeds configured dependency depth");
        return false;
      }
      state[node] = 1;
      for (const auto &edge : graph[node].dependencies) {
        if (!validate(edge.target, depth + 1))
          return false;
        heights[node] = std::max(heights[node], heights[edge.target] + 1);
      }
      state[node] = 2;
      return true;
    };
    for (std::size_t node = 0; node < graph.size(); ++node)
      if (!validate(node, 0))
        return std::move(*failure);
  }
  std::function<std::optional<std::size_t>(fs::path, std::size_t, std::size_t,
                                           std::shared_ptr<const SourceFile>, SourceSpan)>
      visit = [&](fs::path requested, std::size_t owner, std::size_t depth,
                  std::shared_ptr<const SourceFile> parent,
                  SourceSpan span) -> std::optional<std::size_t> {
    if (requested.extension() != ".k") {
      fail(std::move(parent), span, "M001", "source files must use the .k extension");
      return {};
    }
    std::error_code error;
    // Weak canonicalization preserves identities for buffers not yet on disk.
    // Ordinary disk loads retain strict canonicalization and missing-file errors.
    auto canonical =
        overlays.empty() ? fs::canonical(requested, error) : overlayPath(requested, error);
    if (error) {
      fail(std::move(parent), span, "M002", "cannot open module '" + requested.string() + "'");
      return {};
    }
    if (!graph.empty() && owner != 0) {
      const auto relative = canonical.lexically_relative(scopes[owner].root);
      if (relative.empty() || *relative.begin() == ".." || relative.has_root_path()) {
        fail(std::move(parent), span, "M007",
             "module escapes package source root '" + scopes[owner].root.string() + "'");
        return {};
      }
    }
    const auto physical = canonical.string();
    const auto key = std::to_string(owner) + ":" + physical;
    if (auto found = known.find(key); found != known.end()) {
      if (!files[found->second].complete) {
        fail(std::move(parent), span, "M003", "module import cycle reaches '" + physical + "'");
        return {};
      }
      if (depth > moduleLimits.maxDepth ||
          files[found->second].height > moduleLimits.maxDepth - depth) {
        fail(std::move(parent), span, "M004", "module graph exceeds configured import depth");
        return {};
      }
      return found->second;
    }
    if (depth > moduleLimits.maxDepth || files.size() >= moduleLimits.maxFiles ||
        bytes >= syntaxLimits.maxSourceBytes) {
      fail(std::move(parent), span, "M004",
           "module graph exceeds configured depth/file/byte limits");
      return {};
    }
    const auto maximum = syntaxLimits.maxSourceBytes - bytes - 1;
    auto replacement = replacements.find(physical);
    auto source =
        replacement == replacements.end()
            ? loadSource(physical, maximum)
            : std::expected<std::shared_ptr<const SourceFile>, std::string>(replacement->second);
    if (source && (*source)->text().size() > maximum)
      source = std::unexpected("source exceeds the configured size limit");
    if (!source) {
      fail(std::move(parent), span, source.error().starts_with("source exceeds") ? "M004" : "M002",
           "cannot load module '" + physical + "': " + source.error());
      return {};
    }
    auto parsed = parse(*source, syntaxLimits);
    if (!parsed.ok() && !parsed.module.editorOnly) {
      failure = std::move(parsed);
      return {};
    }
    bytes += (*source)->text().size() + 1;
    const auto id = files.size();
    known.emplace(key, id);
    files.push_back({*source, {}, false});
    for (const auto &import : parsed.module.imports) {
      const auto quoted = (*source)->slice(import.path);
      const std::string specifier(quoted.substr(1, quoted.size() - 2));
      fs::path requested;
      auto targetOwner = owner;
      if (specifier.find_first_of("\\\r\n") != std::string::npos ||
          specifier.find('\0') != std::string::npos) {
        fail(*source, import.path, "M001", "module specifiers cannot contain escapes");
        return {};
      }
      if (specifier.starts_with("./") || specifier.starts_with("../")) {
        if (!specifier.ends_with(".k")) {
          fail(*source, import.path, "M001", "relative module paths require an explicit .k file");
          return {};
        }
        requested = canonical.parent_path() / specifier;
      } else {
        if (!packageName(specifier)) {
          fail(*source, import.path, "M001",
               "imports require a relative .k path or declared package name");
          return {};
        }
        auto entry = scopes[owner].entries.find(specifier);
        if (entry == scopes[owner].entries.end()) {
          fail(*source, import.path, "M005",
               graph.empty()
                   ? "unknown local package '" + specifier + "'; supply --package name=path.k"
                   : "unknown package '" + specifier + "' in package '" + graph[owner].identity +
                         "'");
          return {};
        }
        requested = entry->second.first;
        targetOwner = entry->second.second;
      }
      auto target = visit(requested, targetOwner, depth + 1, *source, import.path);
      if (!target)
        return {};
      files[id].targets.push_back(*target);
      files[id].height = std::max(files[id].height, files[*target].height + 1);
    }
    files[id].complete = true;
    return id;
  };
  if (!visit(fs::path(path), 0, 0, root, {}))
    return std::move(*failure);
  if (files.size() == 1) {
    // Preserve the existing single-file source and byte offsets.
    return parse(files[0].source, syntaxLimits);
  }
  std::string text;
  text.reserve(bytes);
  std::vector<SourceFile::Part> parts;
  for (const auto &file : files) {
    parts.push_back({text.size(), file.source});
    text += file.source->text();
    text += '\n';
  }
  auto source = std::make_shared<const SourceFile>(files[0].source->path(), std::move(text),
                                                   std::move(parts));
  auto result = parse(source, syntaxLimits);
  if (!result.ok() && !result.module.editorOnly)
    return result;
  for (auto &function : result.module.functions)
    function.moduleIndex = source->partIndex(function.name.begin);
  std::vector<std::size_t> next(files.size());
  for (auto &import : result.module.imports) {
    import.moduleIndex = source->partIndex(import.path.begin);
    import.target = files[import.moduleIndex].targets[next[import.moduleIndex]++];
  }
  return result;
}
std::expected<ParseResult, std::string> loadModules(std::string path, SyntaxLimits syntaxLimits,
                                                    ModuleLimits moduleLimits,
                                                    std::span<const ModulePackage> packages,
                                                    std::span<const SourceOverlay> overlays) {
  return loadModuleGraph(std::move(path), syntaxLimits, moduleLimits, packages, overlays, {});
}
std::expected<ParseResult, std::string>
loadPackageModules(std::span<const PackageNode> graph, SyntaxLimits syntaxLimits,
                   ModuleLimits moduleLimits, std::span<const SourceOverlay> overlays) {
  if (graph.empty())
    return std::unexpected("offline package graph requires a workspace node");
  return loadModuleGraph(graph[0].entry, syntaxLimits, moduleLimits, {}, overlays, graph);
}
} // namespace kalandra
