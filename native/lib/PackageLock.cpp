#include "kalandra/PackageLock.h"
#include "kalandra/GitRepository.h"
#include "kalandra/GitRevision.h"
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
namespace kalandra {
namespace {
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
bool hex(std::string_view text, std::size_t size) {
  return text.size() == size && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
bool alias(std::string_view text) {
  auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
  return !text.empty() && letter(text.front()) && std::ranges::all_of(text, [&](char c) {
    return letter(c) || (c >= '0' && c <= '9') || c == '-';
  });
}
NativePackageKey key(const toml::table &item) {
  auto repository = item["repository"].value<std::string>();
  auto commit = item["commit"].value<std::string>();
  require(repository && gitTransport(*repository) && commit && hex(*commit, 40),
          "invalid package source identity in kalandra.lock");
  return {*repository, *commit};
}
std::vector<LockedNativeDependency> requests(const toml::node_view<const toml::node> &node) {
  const auto *items = node.as_array();
  require(items && items->size() <= 128, "invalid or oversized dependency list in kalandra.lock");
  std::vector<LockedNativeDependency> result;
  std::set<std::string> names;
  for (const auto &item : *items) {
    const auto *table = item.as_table();
    require(table && table->size() == 4, "invalid dependency record in kalandra.lock");
    auto name = (*table)["name"].value<std::string>();
    require(name && alias(*name) && names.insert(*name).second,
            "invalid or duplicate dependency alias in kalandra.lock");
    auto source = key(*table);
    auto revision = (*table)["rev"].value<std::string>();
    require(revision && validGitRevision(*revision) &&
                (!exactGitCommit(*revision) || *revision == source.second),
            "invalid request or conflicting exact pin in kalandra.lock");
    result.push_back({*name, std::move(source.first), *revision, std::move(source.second)});
  }
  std::ranges::sort(result, {}, &LockedNativeDependency::name);
  return result;
}
bool entryPath(const std::string &text) {
  const std::filesystem::path path(text);
  if (text.empty() || text.size() > 4096 || path.has_root_path() || path.extension() != ".k" ||
      path.generic_string() != text || path.lexically_normal().generic_string() != text ||
      text.find_first_of("\\:\r\n") != std::string::npos || text.find('\0') != std::string::npos)
    return false;
  for (const auto &part : path)
    if (part == "." || part == "..")
      return false;
  return true;
}
toml::array serializeRequests(const std::vector<LockedNativeDependency> &values) {
  auto ordered = values;
  std::ranges::sort(ordered, {}, &LockedNativeDependency::name);
  toml::array result;
  for (const auto &value : ordered)
    result.push_back(toml::table{{"name", value.name},
                                 {"repository", value.repository},
                                 {"rev", value.revision},
                                 {"commit", value.commit}});
  return result;
}
} // namespace
bool sameNativeDependencies(const std::vector<NativeDependency> &left,
                            const std::vector<LockedNativeDependency> &right) {
  return std::ranges::equal(left, right, [](const auto &a, const auto &b) {
    return a.name == b.name && a.repository == b.repository && a.revision == b.revision;
  });
}
std::expected<NativePackageLock, std::string> parseNativePackageLock(std::string_view contents,
                                                                     PackageLockLimits limits) {
  try {
    require(contents.size() <= limits.maxBytes, "kalandra.lock exceeds configured byte limit");
    const auto table = toml::parse(contents);
    require(table.size() == 4 && table["format"].is_integer() && table["format"].value<int>() == 2,
            "invalid kalandra.lock format");
    auto manifest = table["manifest_digest"].value<std::string>();
    const auto *packages = table["packages"].as_array();
    require(manifest && hex(*manifest, 64) && packages && packages->size() <= limits.maxPackages,
            "invalid or oversized kalandra.lock package graph");
    NativePackageLock result{*manifest, requests(table["roots"]), {}};
    for (const auto &node : *packages) {
      const auto *item = node.as_table();
      require(item && item->size() == 5, "invalid package record in kalandra.lock");
      auto source = key(*item);
      auto digest = (*item)["digest"].value<std::string>();
      auto entry = (*item)["entry"].value<std::string>();
      require(digest && hex(*digest, 64) && entry && entryPath(*entry),
              "invalid digest or relative public entry in kalandra.lock");
      require(result.packages
                  .emplace(std::move(source),
                           LockedNativePackage{*digest, *entry, requests((*item)["dependencies"])})
                  .second,
              "duplicate package identity in kalandra.lock");
    }
    std::map<NativePackageKey, std::size_t> heights;
    std::set<NativePackageKey> visiting;
    std::function<std::size_t(const LockedNativeDependency &, std::size_t)> visit =
        [&](const LockedNativeDependency &dependency, std::size_t depth) -> std::size_t {
      require(depth <= limits.maxDepth, "kalandra.lock exceeds configured dependency depth");
      const NativePackageKey source{dependency.repository, dependency.commit};
      if (auto cached = heights.find(source); cached != heights.end()) {
        require(cached->second <= limits.maxDepth - depth,
                "kalandra.lock exceeds configured dependency depth");
        return cached->second;
      }
      require(visiting.insert(source).second, "dependency cycle in kalandra.lock");
      const auto item = result.packages.find(source);
      require(item != result.packages.end(), "missing dependency target in kalandra.lock");
      std::size_t height = 0;
      for (const auto &child : item->second.dependencies)
        height = std::max(height, visit(child, depth + 1) + 1);
      visiting.erase(source);
      heights.emplace(source, height);
      return height;
    };
    for (const auto &dependency : result.roots)
      visit(dependency, 1);
    require(heights.size() == result.packages.size(),
            "unreachable package records in kalandra.lock");
    return result;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::string serializeNativePackageLock(const NativePackageLock &lock) {
  toml::array packages;
  for (const auto &[key, value] : lock.packages)
    packages.push_back(toml::table{{"repository", key.first},
                                   {"commit", key.second},
                                   {"digest", value.digest},
                                   {"entry", value.entry},
                                   {"dependencies", serializeRequests(value.dependencies)}});
  const toml::table table{{"format", 2},
                          {"manifest_digest", lock.manifest},
                          {"roots", serializeRequests(lock.roots)},
                          {"packages", std::move(packages)}};
  std::ostringstream text;
  text << table;
  return text.str();
}
} // namespace kalandra
