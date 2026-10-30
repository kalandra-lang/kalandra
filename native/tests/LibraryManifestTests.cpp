#include "kalandra/Project.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
namespace fs = std::filesystem;
static fs::path root;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void write(const fs::path &file, const std::string &text) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file, std::ios::binary);
  out << text;
  out.close();
  require(bool(out), "fixture write failed");
}
static const std::string base =
    "format=1\n[package]\nname='geometry'\nversion='0.2.0-rc.1+build.001'\nentry='src/public.k'\n";
static std::expected<NativePackage, std::string> load(const std::string &text,
                                                      ProjectLimits limits = {}) {
  write(root / "library/kalandra.toml", text);
  return loadNativePackage((root / "library/kalandra.toml").string(), limits);
}
static std::string replace(std::string text, const std::string &before, const std::string &after) {
  auto pos = text.find(before);
  require(pos != std::string::npos, "bad replacement fixture");
  text.replace(pos, before.size(), after);
  return text;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    const auto valid = base + "[packages]\ninternal='src/helper.k'\n";
    auto library = load(valid);
    require(bool(library), library ? "" : library.error());
    require(library->name == "geometry" && library->version == "0.2.0-rc.1+build.001" &&
                library->manifest == fs::canonical(root / "library/kalandra.toml").string() &&
                library->entry == (root / "library/src/public.k").string() &&
                !library->projectEntry && library->packages.size() == 1 &&
                library->packages[0].entry == (root / "library/src/helper.k").string(),
            "library metadata/path selection failed");
    auto node = library->node("repo@exact-commit", {{"numbers", 2}});
    require(node.identity == "repo@exact-commit" && node.root == (root / "library").string() &&
                node.entry == library->entry && node.dependencies.size() == 1 &&
                node.dependencies[0].target == 2 && node.localPackages.size() == 1,
            "metadata graph adapter lost scope information");
    require(!loadNativeProject(library->manifest), "library-only entry selected as application");
    const auto dual = valid + "[project]\nentry='examples/main.k'\n";
    library = load(dual);
    auto project = loadNativeProject((root / "library/kalandra.toml").string());
    require(library && project && library->projectEntry == project->entry &&
                project->entry == (root / "library/examples/main.k").string() &&
                project->entry != library->entry && project->libraryEntry == library->entry &&
                project->packages.size() == 1,
            "dual manifest entry selection failed");
    write(root / "app.toml", "format=1\n[project]\nentry='missing.k'\n");
    require(!loadNativePackage((root / "app.toml").string()), "application accepted as library");
    for (const auto *version : {"0.0.0", "1.2.3", "999999999999999999999.0.1", "1.0.0-alpha",
                                "1.0.0-alpha.1", "1.0.0-0", "1.0.0-x-y.z+001", "1.0.0+001.02"})
      require(bool(load(replace(base, "0.2.0-rc.1+build.001", version))),
              "valid SemVer metadata rejected: " + std::string(version));
    for (const auto *version :
         {"", "v1.2.3", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-", "1.2.3+",
          "1.2.3-01", "1.2.3-a.00", "1.2.3-a..b", "1.2.3+a..b", "1.2.3+a+b", "1.2.3-hello_world",
          "1.2.3-β", " 1.2.3"})
      require(!load(replace(base, "0.2.0-rc.1+build.001", version)),
              "invalid SemVer metadata accepted: " + std::string(version));
    for (const auto *name : {"_geometry-2", "Geometry"})
      require(bool(load(replace(base, "name='geometry'", "name='" + std::string(name) + "'"))),
              "valid package name rejected");
    for (const auto *name : {"", "1geometry", "a.b", "geometry/name", "name=extra", "géométrie"})
      require(!load(replace(base, "name='geometry'", "name='" + std::string(name) + "'")),
              "invalid package name accepted");
    for (const auto &bad :
         {replace(base, "name='geometry'", "name=42"),
          replace(base, "version='0.2.0-rc.1+build.001'", "version=2"),
          replace(base, "name='geometry'\n", ""),
          replace(base, "version='0.2.0-rc.1+build.001'\n", ""),
          replace(base, "entry='src/public.k'\n", ""),
          replace(base, "entry='src/public.k'", "entry=42"),
          replace(base, "[package]", "package='library'\n[unknown]"), base + "unknown=1\n",
          std::string("format=1\npackage=42\n[project]\nentry='main.k'\n"),
          base + "[dependencies]\nfoo={git='x',rev='main'}\n", base + "[project]\nentry=42\n",
          base + "[project]\nextra=1\n", base + "[packages]\n'name=extra'='src/helper.k'\n"})
      require(!load(bad), "invalid library schema accepted");
    for (const auto *entry :
         {"../escape.k", "/tmp/escape.k", "src/file.kalandra", "", "src\\file.k"})
      require(!load(replace(base, "src/public.k", entry)),
              "invalid/escaping library entry accepted");
    require(!load(base + "[packages]\noutside='../escape.k'\n"),
            "escaping unused library mapping accepted");
    require(!load(valid, {8, 128}) && !load(valid, {256 * 1024, 0}), "manifest limits ignored");
    auto located = load(replace(base, "version='0.2.0-rc.1+build.001'", "version='bad'"));
    require(!located && located.error().find("kalandra.toml:4:") != std::string::npos,
            "metadata diagnostic lost field location");
    require(bool(load(replace(valid, "src/public.k", "src/../src/public.k"))),
            "contained normalized path rejected");
    write(root / "outside.k", "not parsed by metadata loader");
    fs::create_directories(root / "library/src");
    std::error_code error;
    fs::remove(root / "library/src/escape.k", error);
    fs::create_symlink(root / "outside.k", root / "library/src/escape.k");
    require(!load(replace(base, "src/public.k", "src/escape.k")), "symlink entry escape accepted");
    require(!load(base + "[packages]\nunused='src/escape.k'\n"),
            "symlink local mapping escape accepted");
    const std::string dependency =
        "[dependencies]\ngeometrydep={git='/nonexistent/repository',rev='" + std::string(40, 'f') +
        "'}\n";
    auto declared = load(base + dependency);
    require(declared && declared->dependencies.size() == 1 &&
                declared->dependencies[0].name == "geometrydep" &&
                declared->dependencies[0].repository == "/nonexistent/repository" &&
                declared->dependencies[0].revision == std::string(40, 'f'),
            "exact dependency metadata was fetched or lost");
    require(!load(base + dependency, {256 * 1024, 0}), "dependency count limit ignored");
    require(!load(base + "[packages]\ngeometrydep='src/helper.k'\n" + dependency),
            "local/dependency alias conflict accepted");
    require(!loadNativePackage((root / "absent.toml").string()), "missing library admitted");
    library = load(valid);
    require(bool(library), "restore failed");
    fs::create_directories(root / "aliases");
    fs::remove(root / "aliases/library.toml", error);
    fs::create_symlink(root / "library/kalandra.toml", root / "aliases/library.toml");
    auto alias = loadNativePackage((root / "aliases/library.toml").string());
    require(alias && alias->manifest == library->manifest && alias->entry == library->entry,
            "manifest alias changed package root");
    auto fresh = load(replace(base, "0.2.0-rc.1+build.001", "0.3.0"));
    require(fresh && fresh->version == "0.3.0", "library metadata cached across reads");
    std::cout << "library manifests: application/library entries, scoped graph adapter, SemVer, "
                 "strict schemas, confinement, diagnostics, bounded and fresh reads passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
