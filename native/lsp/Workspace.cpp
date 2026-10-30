#include "Workspace.h"
#include "kalandra/Utf8.h"
#include <algorithm>
#include <array>
#include <filesystem>
namespace kalandra::lsp {
namespace fs = std::filesystem;
bool containsPath(std::string_view root, std::string_view path) {
  const auto relative =
      fs::path(path).lexically_normal().lexically_relative(fs::path(root).lexically_normal());
  return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::optional<std::string> containingWorkspace(const std::vector<std::string> &roots,
                                               std::string_view path) {
  std::optional<std::string> selected;
  for (const auto &root : roots)
    if (containsPath(root, path) && (!selected || root.size() > selected->size()))
      selected = root;
  return selected;
}
std::expected<std::vector<std::string>, std::string> discoverSources(const std::string &root,
                                                                     DiscoveryLimits limits) {
  std::error_code ec;
  const auto canonical = fs::canonical(root, ec);
  if (ec || !fs::is_directory(canonical, ec) || ec)
    return std::unexpected("Cannot access workspace directory: " + root);
  const auto excluded = [](const fs::path &path) {
    constexpr std::array names{".git", ".hg",    ".svn",   "node_modules", "build",
                               "dist", "target", ".cache", ".kalandra", ".svelte-kit",  ".vercel"};
    const auto name = path.filename().string();
    return std::ranges::find(names, name) != names.end();
  };
  struct Directory {
    fs::path path;
    std::size_t depth;
  };
  std::vector<Directory> pending{{canonical, 0}};
  std::vector<std::string> sources;
  std::size_t entries = 0;
  while (!pending.empty()) {
    auto directory = std::move(pending.back());
    pending.pop_back();
    fs::directory_iterator iterator(directory.path, ec), end;
    if (ec)
      return std::unexpected("Cannot enumerate workspace directory: " + directory.path.string());
    for (; iterator != end; iterator.increment(ec)) {
      if (ec)
        break;
      if (++entries > limits.maxEntries)
        return std::unexpected("Workspace discovery exceeds directory-entry budget");
      const auto &path = iterator->path();
      const auto status = iterator->symlink_status(ec);
      if (ec)
        return std::unexpected("Cannot inspect workspace entry: " + path.string());
      if (fs::is_symlink(status))
        continue;
      if (fs::is_directory(status)) {
        if (excluded(path))
          continue;
        if (directory.depth >= limits.maxDepth)
          return std::unexpected("Workspace discovery exceeds directory-depth budget");
        pending.push_back({path, directory.depth + 1});
      } else if (fs::is_regular_file(status) && path.extension() == ".k") {
        auto text = path.string();
        if (!validUtf8(text) || text.find_first_of(std::string("\0\r\n", 3)) != text.npos)
          return std::unexpected("Workspace source path cannot be represented as a local URI");
        if (sources.size() >= limits.maxFiles)
          return std::unexpected("Workspace discovery exceeds source-file budget");
        sources.push_back(std::move(text));
      }
    }
    if (ec)
      return std::unexpected("Cannot continue workspace discovery: " + directory.path.string());
  }
  std::ranges::sort(sources);
  return sources;
}
} // namespace kalandra::lsp
