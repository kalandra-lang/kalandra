#include "Workspace.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra::lsp;
namespace fs = std::filesystem;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    const auto base = fs::absolute(argv[1]);
    fs::remove_all(base);
    fs::create_directories(base / "project" / "src");
    fs::create_directories(base / "external");
    const auto root = fs::canonical(base / "project").string();
    const auto touch = [&](const fs::path &path) {
      std::ofstream out(path);
      out << "ignored";
      out.close();
      require(bool(out), "fixture write");
    };
    touch(base / "project" / "main.k");
    touch(base / "project" / "src" / "lib.k");
    touch(base / "project" / "other.txt");
    touch(base / "external" / "hidden.k");
    for (const auto *name : {".git", ".hg", ".svn", "node_modules", "build", "dist", "target",
                             ".cache", ".kalandra", ".svelte-kit", ".vercel"}) {
      fs::create_directories(base / "project" / name / "deep");
      touch(base / "project" / name / "deep" / "skip.k");
    }
    fs::create_directory_symlink(base / "external", base / "project" / "external-link");
    fs::create_symlink(base / "external" / "hidden.k", base / "project" / "link.k");
    fs::create_symlink(base / "external" / "missing.k", base / "project" / "broken.k");
    auto result = discoverSources(root);
    require(result && result->size() == 2 && std::ranges::is_sorted(*result),
            "source discovery/exclusions/symlinks");
    require(containsPath(root, root + "/src/lib.k"), "inside path rejected");
    require(!containsPath(root, root + "-other/a.k"), "sibling prefix admitted");
    require(!containsPath(root, root + "/../external/hidden.k"), "parent traversal admitted");
    require(containingWorkspace({root, root + "/src"}, root + "/src/lib.k") == root + "/src",
            "nested root selection");
    require(!containingWorkspace({root}, (base / "external" / "hidden.k").string()),
            "external root selected");
    DiscoveryLimits limits;
    limits.maxFiles = 2;
    require(discoverSources(root, limits).has_value(), "exact file budget rejected");
    touch(base / "project" / "new.k");
    result = discoverSources(root, limits);
    require(!result && result.error().find("source-file") != std::string::npos,
            "file limit did not fail");
    fs::remove(base / "project" / "new.k");
    limits = {};
    limits.maxEntries = 0;
    require(!discoverSources(root, limits), "entry limit did not fail");
    limits = {};
    limits.maxDepth = 0;
    require(!discoverSources(root, limits), "depth limit did not fail");
    require(!discoverSources((base / "absent").string()), "missing root silently ignored");
    require(!discoverSources((base / "project" / "main.k").string()), "file root accepted");
    fs::remove(base / "project" / "src" / "lib.k");
    result = discoverSources(root);
    require(result && result->size() == 1, "deletion not discovered");
    touch(base / "project" / "fresh.k");
    require(discoverSources(root)->size() == 2, "new source not discovered");
    std::cout << "Native workspace discovery: exclusions, symlinks, boundaries, nested roots, "
                 "fresh sources and explicit budget failures passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
