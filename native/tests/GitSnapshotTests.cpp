#include "kalandra/Backend.h"
#include "kalandra/GitSnapshot.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef KALANDRA_SNAPSHOT_FRONTEND_ONLY
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#endif
using namespace kalandra;
namespace fs = std::filesystem;
static void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::string read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
static void write(const fs::path &path, const std::string &text) {
  std::ofstream output(path, std::ios::binary);
  output << text;
  output.close();
  require(bool(output), "fixture write failed");
}
static NativeGitSnapshot install(const fs::path &cache, const GitSnapshotRequest &request) {
  auto result = installLocalGitSnapshot(cache.string(), request);
  require(bool(result), result ? "" : result.error());
  return std::move(*result);
}
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  try {
    const fs::path root = fs::canonical(argv[1]);
    const std::string action = argv[2];
    const auto ids = read(root / "commits");
    std::vector<std::string> commits;
    for (std::size_t offset = 0; offset < ids.size();) {
      auto end = ids.find('\n', offset);
      require(end != std::string::npos, "invalid fixture commits");
      commits.push_back(ids.substr(offset, end - offset));
      offset = end + 1;
    }
    require(commits.size() == 8, "wrong fixture commit count");
    GitSnapshotRequest first{(root / "repository").string(), commits[0]};
    GitSnapshotRequest second{first.repository, commits[1]};
    const fs::path cache = root / argv[3];
    fs::create_directories(cache);
    if (action == "race") {
      std::cout << install(cache, first).digest << '\n';
      return 0;
    }
    if (action == "offline") {
      auto loaded = loadLocalGitSnapshot(cache.string(), first);
      require(bool(loaded), loaded ? "" : loaded.error());
      std::cout << loaded->digest << '\n';
      return 0;
    }
    const auto old = install(cache, first);
    const auto current = install(cache, second);
    require(old.identity != current.identity && old.digest != current.digest,
            "different commits lost source identity");
    require(install(cache, first).digest == old.digest, "snapshot reuse changed contents");
    require(!fs::exists(fs::path(old.directory) / "source/.git"),
            "repository metadata copied to source");
    require(read(fs::path(old.directory) / "source/literal.txt") == "$Format:%H$\n",
            "export substitution changed raw source contents");
    require(read(fs::path(old.directory) / "source/retained.txt") == "retained\n",
            "export-ignore dropped source files");
    require((fs::status(fs::path(old.directory) / "source/tool.sh").permissions() &
             fs::perms::owner_exec) == fs::perms::none,
            "snapshot preserved executable file mode");
    const auto telemetry = install(cache, {(root / "telemetry").string(), commits[2]});
    const auto packed = install(cache, {(root / "bare").string(), commits[1]});
    require(packed.digest == current.digest && packed.identity != current.identity,
            "packed repository content/identity incorrect");
    write(root / "main.k", "from \"geometry\" import Item, next\nfrom \"telemetry\" import "
                           "compute\nfn main()->[int64] ! TaskError:\n  old=try parallel:\n    try "
                           "await spawn compute(10)\n  [next(Item {value:10}).value,old]\n");
    std::vector<PackageNode> graph = {{"workspace",
                                       root.string(),
                                       (root / "main.k").string(),
                                       {{"geometry", 1}, {"telemetry", 2}},
                                       {}},
                                      current.library.node(current.identity),
                                      telemetry.library.node(telemetry.identity, {{"geometry", 3}}),
                                      old.library.node(old.identity)};
    auto parsed = loadPackageModules(graph);
    require(parsed && parsed->ok(), "snapshot graph could not load");
    auto typed = check(std::move(*parsed));
    require(typed.ok() && typed.module->records.size() == 2, "snapshot version scopes failed");
#ifndef KALANDRA_SNAPSHOT_FRONTEND_ONLY
    auto lowered = lowerNative(*typed.module);
    require(bool(lowered), lowered ? "" : lowered.error());
    write(root / "scoped.ll", lowered->llvm);
    for (bool thread : {false, true}) {
      llvm::LLVMContext context;
      llvm::SMDiagnostic diagnostic;
      auto module = llvm::parseAssemblyString(lowered->llvm, diagnostic, context);
      require(bool(module), "generated snapshot LLVM parse failed");
      for (auto &function : *module)
        if (!function.isDeclaration())
          function.addFnAttr(thread ? llvm::Attribute::SanitizeThread
                                    : llvm::Attribute::SanitizeAddress);
      require(!llvm::verifyModule(*module, &llvm::errs()),
              "generated snapshot LLVM verification failed");
      std::string text;
      llvm::raw_string_ostream stream(text);
      module->print(stream, nullptr);
      write(root / (thread ? "scoped-thread-sanitized.ll" : "scoped-sanitized.ll"), text);
    }
#endif
    const auto published = std::distance(fs::directory_iterator(cache), fs::directory_iterator{});
    for (const auto &request :
         std::vector<GitSnapshotRequest>{{first.repository, "main"},
                                         {first.repository, std::string(40, 'f')},
                                         {first.repository, commits[3]},
                                         {first.repository, commits[4]},
                                         {first.repository, commits[5]},
                                         {first.repository, commits[6]},
                                         {first.repository, commits[7]},
                                         {(root / "alternate").string(), commits[0]},
                                         {(root / "broken").string(), commits[0]}}) {
      require(!installLocalGitSnapshot(cache.string(), request), "invalid Git snapshot admitted");
      require(std::distance(fs::directory_iterator(cache), fs::directory_iterator{}) == published,
              "rejected snapshot changed published cache entries or leaked staging");
    }
    const std::vector<GitSnapshotLimits> limits = {{.maxObjectFiles = 1},   {.maxObjectBytes = 1},
                                                   {.maxSourceEntries = 1}, {.maxSourceBytes = 1},
                                                   {.maxListingBytes = 1},  {.processSeconds = 0}};
    // Every rejection has a fresh cache so a prior verified snapshot cannot mask extraction limits.
    fs::create_directory(root / "limited");
    for (const auto &limit : limits) {
      require(!installLocalGitSnapshot((root / "limited").string(), first, limit),
              "snapshot limit ignored");
      require(fs::is_empty(root / "limited"), "failed extraction left published or staged files");
    }
    auto source = fs::path(old.directory) / "source/public.k";
    const auto original = read(source);
    write(source, original + "\n# changed\n");
    require(!loadLocalGitSnapshot(cache.string(), first) &&
                !installLocalGitSnapshot(cache.string(), first),
            "modified snapshot was accepted or silently repaired");
    require(read(source) == original + "\n# changed\n",
            "reinstallation replaced a corrupt existing snapshot");
    write(source, original);
    const auto info = fs::path(old.directory) / "snapshot.toml";
    const auto metadata = read(info);
    write(info, metadata + "\nunknown=1\n");
    require(!loadLocalGitSnapshot(cache.string(), first), "unknown snapshot metadata accepted");
    write(info, metadata);
    const auto extra = fs::path(old.directory) / "source/extra";
    write(extra, "extra");
    require(!loadLocalGitSnapshot(cache.string(), first),
            "extra source file escaped integrity checking");
    fs::remove(extra);
    fs::create_directory(extra);
    require(!loadLocalGitSnapshot(cache.string(), first),
            "extra empty directory escaped integrity checking");
    fs::remove(extra);
    fs::create_symlink(source, extra);
    require(!loadLocalGitSnapshot(cache.string(), first), "snapshot symlink accepted");
    fs::remove(extra);
    require(bool(loadLocalGitSnapshot(cache.string(), first)),
            "restored snapshot failed verification");
    for (const auto &entry : fs::directory_iterator(cache))
      require(!entry.path().filename().string().starts_with(".kalandra-snapshot-"),
              "private staging leaked");
    fs::create_directory(root / "redirected");
    fs::create_symlink(old.directory, root / "redirected" / fs::path(old.directory).filename());
    require(!installLocalGitSnapshot((root / "redirected").string(), first),
            "preexisting snapshot symlink followed");
    require(!fs::exists(root / "executed"), "Git executed repository/environment commands");
    std::cout
        << "Git snapshots: pinned objects, raw blobs, packed stores, offline scoped workers, "
           "integrity, bounded failures, no replacements and no repository execution passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
