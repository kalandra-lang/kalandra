#include "kalandra/PackageInstall.h"
#include "PlatformFileSystem.h"
#include "kalandra/GitRevision.h"
#include "kalandra/PackageLock.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>
#include <map>
#include <stdexcept>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace kalandra {
namespace {
namespace fs = std::filesystem;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string hash(std::string_view contents) {
  llvm::SHA256 hash;
  hash.update(llvm::StringRef(contents));
  std::string result;
  for (auto c : hash.final()) {
    result += "0123456789abcdef"[c >> 4];
    result += "0123456789abcdef"[c & 15];
  }
  return result;
}
std::string read(const fs::path &path, std::size_t maximum) {
  require(fs::is_regular_file(fs::symlink_status(path)),
          "package metadata must be a regular file: " + path.string());
  require(fs::file_size(path) <= maximum, "package metadata exceeds configured byte limit");
  std::ifstream input(path, std::ios::binary);
  require(bool(input), "cannot read package metadata");
  std::string result;
  char buffer[8192];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    require(std::size_t(input.gcount()) <= maximum - result.size(),
            "package metadata exceeds configured byte limit");
    result.append(buffer, std::size_t(input.gcount()));
  }
  require(input.eof(), "cannot read package metadata");
  return result;
}
fs::path cache(const NativeProject &project) {
  return fs::path(project.manifest).parent_path() / ".kalandra";
}
void directory(const fs::path &path, bool create) {
  if (create && !fs::exists(fs::symlink_status(path)))
    require(!detail::createPrivateDirectory(path) || fs::is_directory(fs::symlink_status(path)),
            "cannot create private package cache directory");
  require(fs::is_directory(fs::symlink_status(path)),
          "package cache must use real directories: " + path.string());
}
using Key = NativePackageKey;
fs::path lockPath(const NativeProject &project) {
  return fs::path(project.manifest).parent_path() / "kalandra.lock";
}
NativePackageLock lock(std::string_view contents, PackageInstallLimits limits) {
  auto parsed =
      parseNativePackageLock(contents, {limits.maxLockBytes, limits.maxPackages, limits.maxDepth});
  require(bool(parsed), parsed ? "" : parsed.error());
  return std::move(*parsed);
}
struct Resolved {
  InstalledNativeProject graph;
  NativePackageLock selected;
};
Resolved resolve(const NativeProject &project, const NativePackageLock &active, bool fetch,
                 bool validateSources, bool strict, bool allowResolution,
                 PackageInstallLimits limits, std::span<const std::string> updateRoots = {}) {
  require(!strict || sameNativeDependencies(project.dependencies, active.roots),
          "dependency declarations differ from kalandra.lock; run kalandra install");
  const auto root = cache(project);
  Resolved result{{{{"workspace",
                     fs::path(project.manifest).parent_path().string(),
                     project.entry,
                     {},
                     project.packages}}},
                  {hash(project.manifestContents), {}, {}}};
  std::map<Key, std::size_t> known;
  std::map<Key, std::string> resolvedRequests;
  std::vector<bool> complete{false};
  std::vector<std::size_t> heights{0};
  std::size_t entries = 0, bytes = 0;
  const std::vector<LockedNativeDependency> noPrior;
  using Visit = std::pair<std::size_t, LockedNativeDependency>;
  std::function<Visit(const NativeDependency &, const std::vector<LockedNativeDependency> &,
                      std::size_t)>
      visit = [&](const NativeDependency &dependency,
                  const std::vector<LockedNativeDependency> &prior, std::size_t depth) -> Visit {
    require(depth <= limits.maxDepth, "package dependencies exceed configured depth");
    const auto pinned = std::ranges::find_if(prior, [&](const auto &item) {
      return item.name == dependency.name && item.repository == dependency.repository &&
             item.revision == dependency.revision;
    });
    std::string commit;
    std::optional<NativeGitSnapshot> materialized;
    if (pinned != prior.end())
      commit = pinned->commit;
    else {
      require(allowResolution, "dependency request is not pinned in kalandra.lock");
      if (exactGitCommit(dependency.revision))
        commit = dependency.revision;
      else {
        const Key request{dependency.repository, dependency.revision};
        if (auto previous = resolvedRequests.find(request); previous != resolvedRequests.end())
          commit = previous->second;
        else {
          require(known.size() < limits.maxPackages,
                  "package graph exceeds configured package limit");
          auto resolved = installGitRevision((root / "snapshots").string(), dependency.repository,
                                             dependency.revision, limits.snapshot);
          require(bool(resolved), resolved ? "" : resolved.error());
          commit = resolved->commit;
          materialized = std::move(resolved->snapshot);
          resolvedRequests.emplace(request, commit);
        }
      }
    }
    const Key key{dependency.repository, commit};
    LockedNativeDependency edge{dependency.name, dependency.repository, dependency.revision,
                                commit};
    if (auto found = known.find(key); found != known.end()) {
      require(complete[found->second],
              "package dependency cycle reaches '" + dependency.name + "'");
      require(heights[found->second] <= limits.maxDepth - depth,
              "package dependencies exceed configured depth");
      return {found->second, std::move(edge)};
    }
    require(known.size() < limits.maxPackages, "package graph exceeds configured package limit");
    const auto selected = active.packages.find(key);
    require(allowResolution || selected != active.packages.end(),
            "dependency target is not pinned in kalandra.lock");
    if (!materialized) {
      auto loaded = fetch ? installGitSnapshot((root / "snapshots").string(),
                                               {key.first, key.second}, limits.snapshot)
                          : loadGitSnapshot((root / "snapshots").string(), {key.first, key.second},
                                            limits.snapshot);
      require(bool(loaded), loaded ? "" : loaded.error());
      materialized = std::move(*loaded);
    }
    auto &snapshot = *materialized;
    LockedNativePackage record{snapshot.digest,
                               fs::path(snapshot.library.entry)
                                   .lexically_relative(fs::path(snapshot.directory) / "source")
                                   .generic_string(),
                               {}};
    if (selected != active.packages.end())
      require(
          record.digest == selected->second.digest && record.entry == selected->second.entry &&
              sameNativeDependencies(snapshot.library.dependencies, selected->second.dependencies),
          "installed snapshot does not match kalandra.lock digest, entry or dependencies");
    for (const auto &entry :
         fs::recursive_directory_iterator(fs::path(snapshot.directory) / "source")) {
      require(entries++ < limits.maxSourceEntries,
              "package graph exceeds configured source entry limit");
      if (entry.is_regular_file()) {
        const auto size = entry.file_size();
        require(size <= limits.maxSourceBytes - bytes,
                "package graph exceeds configured source byte limit");
        bytes += size;
      }
    }
    require(fs::is_regular_file(fs::symlink_status(snapshot.library.entry)),
            "library public entry is missing");
    for (const auto &local : snapshot.library.packages)
      require(fs::is_regular_file(fs::symlink_status(local.entry)),
              "library local mapping entry is missing");
    const auto id = result.graph.nodes.size();
    known.emplace(key, id);
    result.selected.packages.emplace(key, std::move(record));
    result.graph.nodes.push_back(snapshot.library.node(snapshot.identity));
    complete.push_back(false);
    heights.push_back(0);
    for (const auto &child : snapshot.library.dependencies) {
      auto [target, childPin] =
          visit(child, selected == active.packages.end() ? noPrior : selected->second.dependencies,
                depth + 1);
      result.graph.nodes[id].dependencies.push_back({child.name, target});
      result.selected.packages.at(key).dependencies.push_back(std::move(childPin));
      heights[id] = std::max(heights[id], heights[target] + 1);
    }
    require(selected == active.packages.end() ||
                result.selected.packages.at(key).dependencies == selected->second.dependencies,
            "resolved dependencies disagree with kalandra.lock pins");
    complete[id] = true;
    return {id, std::move(edge)};
  };
  for (const auto &dependency : project.dependencies) {
    const bool update = std::ranges::find(updateRoots, dependency.name) != updateRoots.end();
    auto [target, pin] = visit(dependency, update ? noPrior : active.roots, 1);
    result.graph.nodes[0].dependencies.push_back({dependency.name, target});
    result.selected.roots.push_back(std::move(pin));
  }
  require(!strict || result.selected.packages.size() == active.packages.size(),
          "kalandra.lock contains unexpected graph nodes");
  if (validateSources) {
    auto parsed = loadPackageModules(result.graph.nodes, {}, {128, limits.maxDepth});
    require(bool(parsed), parsed ? "" : parsed.error());
    if (!parsed->ok())
      throw std::runtime_error(
          renderDiagnostic(*parsed->module.source, parsed->diagnostics.front()));
  }
  return result;
}
struct Guard {
#ifdef _WIN32
  HANDLE fd = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif
  fs::path temporary;
  ~Guard() {
    if (!temporary.empty()) {
      std::error_code ignored;
      fs::remove_all(temporary, ignored);
    }
#ifdef _WIN32
    if (fd != INVALID_HANDLE_VALUE)
      CloseHandle(fd);
#else
    if (fd >= 0)
      ::close(fd);
#endif
  }
};
bool within(const fs::path &path, const fs::path &root) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.has_root_path() && *relative.begin() != "..";
}
fs::path outputIdentity(fs::path path) {
  // weakly_canonical alone leaves a dangling final symlink unresolved. Follow
  // that link too so outputs cannot create a missing reserved lock through it.
  for (unsigned links = 0; links < 40; ++links) {
    path = fs::weakly_canonical(path.parent_path()) / path.filename();
    if (!fs::is_symlink(fs::symlink_status(path)))
      return path.lexically_normal();
    const auto target = fs::read_symlink(path);
    path = (target.is_absolute() ? target : path.parent_path() / target).lexically_normal();
  }
  throw std::runtime_error("package output path contains too many symbolic links");
}
} // namespace
std::expected<void, std::string> installNativeProject(std::string manifest,
                                                      PackageInstallLimits limits,
                                                      PackageInstallOptions options) {
  try {
    auto project = loadNativeProject(std::move(manifest));
    require(bool(project), project ? "" : project.error());
    const auto destination = lockPath(*project);
    std::optional<std::string> before;
    NativePackageLock active;
    if (fs::exists(fs::symlink_status(destination))) {
      before = read(destination, limits.maxLockBytes);
      active = lock(*before, limits);
    }
    const bool matching = before && active.manifest == hash(project->manifestContents);
    if (!options.updateRoots.empty()) {
      require(!options.frozen && !options.offline, "named updates cannot use frozen/offline modes");
      require(matching && sameNativeDependencies(project->dependencies, active.roots),
              "named update requires a matching kalandra.lock; run kalandra install first");
      require(options.updateRoots.size() <= 128, "too many dependency names to update");
      for (std::size_t i = 0; i < options.updateRoots.size(); ++i) {
        const auto &name = options.updateRoots[i];
        require(std::find(options.updateRoots.begin(), options.updateRoots.begin() + i, name) ==
                    options.updateRoots.begin() + i,
                "duplicate dependency update name '" + name + "'");
        require(
            std::ranges::any_of(project->dependencies,
                                [&](const auto &dependency) { return dependency.name == name; }),
            "unknown managed root dependency '" + name + "'");
      }
    }
    require((!options.frozen && !options.offline) || matching,
            "frozen/offline install requires a matching kalandra.lock");
    const auto root = cache(*project);
    directory(root, !options.offline);
    Guard guard;
#ifdef _WIN32
    guard.fd = CreateFileW((root / "install.guard").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(guard.fd != INVALID_HANDLE_VALUE, "cannot acquire package installation guard");
    BY_HANDLE_FILE_INFORMATION status{};
    require(
        GetFileInformationByHandle(guard.fd, &status) && status.nNumberOfLinks == 1 &&
            !(status.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)),
        "package installation guard must be a private regular file");
#else
    guard.fd =
        ::open((root / "install.guard").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    require(guard.fd >= 0, "cannot open package installation guard");
    struct stat status;
    require(::fstat(guard.fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1,
            "package installation guard must be a private regular file");
    require(::flock(guard.fd, LOCK_EX | LOCK_NB) == 0,
            "another package installation is in progress");
#endif
    directory(root / "snapshots", !options.offline);
    auto resolved =
        resolve(*project, active, !options.offline, true, matching && options.updateRoots.empty(),
                !options.frozen && !options.offline, limits, options.updateRoots);
    const auto contents = serializeNativePackageLock(resolved.selected);
    // Validate generated records too; no unchecked graph is ever activated.
    lock(contents, limits);
    const auto unchanged = [&] {
      require(read(project->manifest, 256 * 1024) == project->manifestContents,
              "project manifest changed during installation; activation cancelled");
      if (before)
        require(read(destination, limits.maxLockBytes) == *before,
                "kalandra.lock changed during installation; activation cancelled");
      else
        require(!fs::exists(fs::symlink_status(destination)),
                "kalandra.lock appeared during installation; activation cancelled");
    };
    if (options.frozen || options.offline) {
      unchanged();
      return {}; // Neither mode rewrites even the lock's formatting or comments.
    }
#ifdef _WIN32
    require(!detail::createPrivateStage(root, ".activate-", guard.temporary),
            "cannot stage package lock activation");
    const auto staged = guard.temporary / "kalandra.lock";
    std::ofstream file(staged, std::ios::binary);
    file.write(contents.data(), std::streamsize(contents.size()));
    file.close();
    require(bool(file), "cannot write package lock activation");
    unchanged();
    require(!detail::replaceFile(staged, destination), "cannot activate kalandra.lock");
#else
    auto name = (root / ".activate-XXXXXX").string();
    const int fd = ::mkstemp(name.data());
    require(fd >= 0, "cannot stage package lock activation");
    guard.temporary = name;
    std::size_t written = 0;
    while (written < contents.size()) {
      const auto count = ::write(fd, contents.data() + written, contents.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        ::close(fd);
        throw std::runtime_error("cannot write package lock activation");
      }
      written += std::size_t(count);
    }
    require(::close(fd) == 0, "cannot close package lock activation");
    unchanged();
    require(::rename(guard.temporary.c_str(), destination.c_str()) == 0,
            "cannot activate kalandra.lock");
    guard.temporary.clear();
#endif
    return {};
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::expected<InstalledNativeProject, std::string>
loadInstalledNativeProject(const NativeProject &project, PackageInstallLimits limits) {
  try {
    const auto root = cache(project);
    if (project.dependencies.empty())
      return InstalledNativeProject{{{"workspace",
                                      fs::path(project.manifest).parent_path().string(),
                                      project.entry,
                                      {},
                                      project.packages}}};
    const auto destination = lockPath(project);
    require(fs::exists(fs::symlink_status(destination)),
            "dependencies have no kalandra.lock; run kalandra install --project " +
                project.manifest);
    const auto active = lock(read(destination, limits.maxLockBytes), limits);
    require(active.manifest == hash(project.manifestContents),
            "project manifest differs from kalandra.lock; run kalandra install --project " +
                project.manifest);
    directory(root, false);
    directory(root / "snapshots", false);
    return resolve(project, active, false, false, true, false, limits).graph;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::expected<void, std::string> protectNativePackageOutput(const NativeProject &project,
                                                            std::string output,
                                                            PackageInstallLimits limits) {
  try {
    const auto root = cache(project);
    const auto target = fs::absolute(output).lexically_normal();
    const auto canonical = outputIdentity(target);
    const auto publicLock = lockPath(project);
    std::error_code lockError;
    require(!within(target, publicLock) && !within(canonical, outputIdentity(publicLock)) &&
                !fs::equivalent(target, publicLock, lockError),
            "package lock and output paths must be distinct");
    require(!within(target, root) && !within(canonical, root),
            "package cache and output paths must be distinct");
    if (!fs::exists(fs::symlink_status(root)))
      return {};
    directory(root, false);
    std::size_t entries = 0;
    for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
      const auto &entry = *iterator;
      const auto name = entry.path().filename().string();
      if ((entry.path().parent_path() == root / "snapshots" &&
           name.starts_with(".kalandra-snapshot-")) ||
          (entry.path().parent_path() == root && name.starts_with(".activate-"))) {
        iterator.disable_recursion_pending();
        continue;
      }
      require(entries++ < limits.maxSourceEntries,
              "package cache exceeds output-protection entry limit");
      std::error_code error;
      require(!fs::equivalent(target, entry.path(), error),
              "package cache and output paths must be distinct");
    }
    return {};
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
} // namespace kalandra
