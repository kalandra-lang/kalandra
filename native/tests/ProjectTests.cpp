#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
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
  require(bool(out), "write failed");
}
static std::expected<NativeProject, std::string> load(const std::string &text,
                                                      ProjectLimits limits = {}) {
  write(root / "project/kalandra.toml", text);
  return loadNativeProject((root / "project/kalandra.toml").string(), limits);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    write(root / "project/src/main.k",
          "from \"geometry\" import Point, total\nfn main()->int ! TaskError:\n  try parallel:\n   "
          " try await spawn total(Point {x:20,y:22})\n");
    write(root / "library/public.k",
          "export type Point:\n  x:int\n  y:int\nexport fn total(p:Point)->int:\n  p.x+p.y\n");
    const std::string valid = "format = 1\n[project]\nentry = 'src/main.k'\n[packages]\ngeometry = "
                              "'../library/public.k'\n";
    auto project = load(valid);
    require(bool(project), project ? "" : project.error());
    require(project->manifest == fs::canonical(root / "project/kalandra.toml").string() &&
                project->entry == (root / "project/src/main.k").string() &&
                project->packages.size() == 1 &&
                project->packages[0].entry == (root / "library/public.k").string(),
            "manifest paths did not resolve from canonical manifest directory");
    auto parsed = loadModules(project->entry, {}, {}, project->packages);
    require(bool(parsed) && parsed->ok(), "project graph failed loading");
    auto checked = check(std::move(*parsed));
    require(checked.ok(), "project graph failed checking");
    require(bool(continuation::plan(*checked.module)), "project worker plan failed");
#ifndef KALANDRA_PROJECT_FRONTEND_ONLY
    auto lowered = lowerNative(*checked.module);
    require(bool(lowered), lowered ? "" : lowered.error());
#endif
    for (const auto &bad :
         {"[project]\nentry='main.k'\n",
          "format=2\n[project]\nentry='main.k'\n",
          "format=1.0\n[project]\nentry='main.k'\n",
          "format=true\n[project]\nentry='main.k'\n",
          "format=1\n",
          "format=1\nproject='main.k'\n",
          "format=1\n[project]\nentry=1\n",
          "format=1\n[project]\nentry='/tmp/main.k'\n",
          "format=1\n[project]\nentry='main.kalandra'\n",
          "format=1\n[project]\nentry=''\n",
          "format=1\n[project]\nentry='main.k'\nother=1\n",
          "format=1\n[build]\nsource='src'\n",
          "format=1\nformat=1\n[project]\nentry='main.k'\n",
          "format=1\n[project]\nentry='main.k'\npackages=[]\n",
          "format=1\npackages=[]\n[project]\nentry='main.k'\n",
          "format=1\n[project]\nentry='main.k'\n[packages]\n'a.b'='lib.k'\n",
          "format=1\n[project]\nentry='main.k'\n[packages]\nlib=1\n",
          "format=1\n[project]\nentry='main.k'\n[packages]\nlib='/tmp/lib.k'\n",
          "format=1\n[project]\nentry='main.k'\n[packages]\nlib='lib.k'\nlib='other.k'\n",
          "format=1\n[project]\nentry=\"bad\\u0000.k\"\n",
          "format=1\n[project]\nentry='src\\main.k'\n",
          "format=1\n[project]\nentry='main.k'\n[packages]\n[packages.lib]\nentry='lib.k'\n"})
      require(!load(bad), "invalid manifest schema/TOML admitted");
    auto located = load("format=1\n[project]\nentry=42\n");
    require(!located && located.error().find("kalandra.toml:3:") != std::string::npos,
            "manifest diagnostic lost path/location");
    require(!load(valid, {8, 128}), "manifest byte budget ignored");
    require(!load(valid, {256 * 1024, 0}), "manifest package budget ignored");
    require(bool(load("# "
                      "Comment\r\nformat=1\r\n[project]\r\nentry=\"src/../src/"
                      "main.k\"\r\n[packages]\r\ngeometry='../library/public.k'\r\n")),
            "ordinary TOML comments/CRLF rejected");
    auto unused = load("format=1\n[project]\nentry='missing.k'\n[packages]\nunused='absent.k'\n");
    require(bool(unused), "manifest loader eagerly loaded .k sources");
    auto noFile = loadNativeProject((root / "absent.toml").string());
    require(!noFile, "missing manifest admitted");
    write(root / "wrong.json", valid);
    require(!loadNativeProject((root / "wrong.json").string()), "non-TOML extension admitted");
    project = load(valid);
    require(bool(project), "restore failed");
    std::error_code error;
    fs::create_directories(root / "aliases");
    fs::remove(root / "aliases/project.toml", error);
    fs::create_symlink(root / "project/kalandra.toml", root / "aliases/project.toml");
    auto alias = loadNativeProject((root / "aliases/project.toml").string());
    require(bool(alias) && alias->entry == project->entry,
            "manifest symlink changed relative base");
    auto fresh = load("format=1\n[project]\nentry='other.k'\n");
    require(bool(fresh) && fresh->entry == (root / "project/other.k").string(),
            "manifest edits cached across loads");
    write(root / "project/kalandra.toml", valid + std::string(256 * 1024, ' '));
    require(!loadNativeProject((root / "project/kalandra.toml").string()),
            "default byte limit ignored");
    std::cout << "native project manifests: versioned TOML, canonical relative paths, shared "
                 "graph/worker plans, strict schema, original diagnostics, limits, symlinks and "
                 "fresh reads passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
