#include "kalandra/GitSnapshot.h"
#include "PlatformFileSystem.h"
#include "WindowsGitProcess.h"
#include "kalandra/GitRepository.h"
#include "kalandra/GitRevision.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>
#include <map>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>
#ifndef _WIN32
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <unordered_set>
namespace kalandra {
namespace {
namespace fs = std::filesystem;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
bool hex(std::string_view text, std::size_t size) {
  return text.size() == size && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
std::string digest(llvm::SHA256 &hash) {
  std::string result;
  for (auto byte : hash.final()) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
void field(llvm::SHA256 &hash, std::string_view text) {
  const auto size = std::to_string(text.size()) + ":";
  hash.update(size);
  hash.update(llvm::StringRef(text.data(), text.size()));
}
std::string requestKey(const GitSnapshotRequest &request) {
  const auto transport = gitTransport(request.repository);
  require(bool(transport), transport ? "" : transport.error());
  require(hex(request.commit, 40), "snapshot requires a full lowercase 40-digit Git commit");
  llvm::SHA256 hash;
  field(hash,
        *transport == GitTransport::Local ? "kalandra-local-git-v1" : "kalandra-remote-git-v1");
  field(hash, request.repository);
  field(hash, request.commit);
  return digest(hash);
}
fs::path cacheRoot(const std::string &path) {
  require(!path.empty() && path.find('\0') == std::string::npos,
          "snapshot cache requires a directory path");
  require(!fs::is_symlink(fs::symlink_status(path)) && fs::is_directory(path),
          "snapshot cache must be an existing directory, not a symlink");
  return fs::canonical(path);
}
std::string read(const fs::path &path, std::size_t maximum) {
  require(fs::is_regular_file(fs::symlink_status(path)),
          "snapshot file is not regular: " + path.string());
  require(fs::file_size(path) <= maximum, "snapshot file exceeds configured byte limit");
  std::ifstream input(path, std::ios::binary);
  require(bool(input), "cannot read snapshot file");
  std::string result;
  char buffer[16384];
  while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
    require(std::size_t(input.gcount()) <= maximum - result.size(),
            "snapshot file exceeds configured byte limit");
    result.append(buffer, std::size_t(input.gcount()));
  }
  require(input.eof(), "cannot read snapshot file");
  return result;
}
void write(const fs::path &path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  output.write(text.data(), std::streamsize(text.size()));
  output.close();
  require(bool(output), "cannot write snapshot staging file");
}
struct Stage {
  fs::path path;
  explicit Stage(const fs::path &parent) {
    require(!detail::createPrivateStage(parent, ".kalandra-snapshot-", path),
            "cannot create private snapshot staging directory");
  }
  ~Stage() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
void objectBudget(const fs::path &database, GitSnapshotLimits limits) {
  std::size_t entries = 0, bytes = 0;
  for (const auto &entry : fs::recursive_directory_iterator(database / "objects")) {
    std::error_code statusError;
    const auto status = entry.symlink_status(statusError);
    if (statusError == std::errc::no_such_file_or_directory)
      continue;
    require(!statusError, "cannot inspect downloaded Git store entry");
    require(fs::is_directory(status) || fs::is_regular_file(status),
            "downloaded Git store contains unsupported entries");
    require(entries++ < limits.maxObjectFiles,
            "downloaded Git store exceeds configured entry limit");
    if (fs::is_regular_file(status)) {
      std::error_code error;
      const auto size = fs::file_size(entry.path(), error);
      // Git can rename its own temporary pack/index while it is being observed.
      require(!error || error == std::errc::no_such_file_or_directory,
              "cannot inspect downloaded Git store");
      if (!error) {
        require(size <= limits.maxObjectBytes - bytes,
                "downloaded Git store exceeds configured byte limit");
        bytes += size;
      }
    }
  }
}
// Only our private object database is used. Fetch permits just the selected
// HTTP(S) transport; plumbing permits none. No inherited config or credentials.
std::string git(const fs::path &database, std::vector<std::string> arguments, std::size_t maximum,
                unsigned seconds, const GitSnapshotLimits *network = nullptr,
                GitTransport transport = GitTransport::Local) {
  require(seconds > 0, "Git process timeout must be nonzero");
#ifdef _WIN32
  const auto gitExecutable = detail::windowsGit();
  const std::string gitPath = gitExecutable.string(), nullPath = "NUL", askPass = "NUL";
#else
  const std::string gitPath = "/usr/bin/git", nullPath = "/dev/null", askPass = "/usr/bin/false";
#endif
  std::vector<std::string> command = {gitPath,
                                      "--no-pager",
                                      "--no-replace-objects",
                                      "--git-dir=" + database.string(),
                                      "-c",
                                      "core.hooksPath=" + nullPath,
                                      "-c",
                                      "protocol.allow=never",
                                      "-c",
                                      "core.fsmonitor=false"};
  if (network) {
    command.insert(command.end(), {"-c",
                                   transport == GitTransport::HTTPS ? "protocol.https.allow=always"
                                                                    : "protocol.http.allow=always",
                                   "-c",
                                   "http.followRedirects=false",
                                   "-c",
                                   "http.sslVerify=true",
                                   "-c",
                                   "credential.helper=",
                                   "-c",
                                   "credential.interactive=false",
                                   "-c",
                                   "core.askPass=" + askPass,
                                   "-c",
                                   "gc.auto=0",
                                   "-c",
                                   "maintenance.auto=false",
                                   "-c",
                                   "fetch.writeCommitGraph=false",
                                   "-c",
                                   "fetch.fsckObjects=true",
                                   "-c",
                                   "transfer.fsckObjects=true",
                                   "-c",
                                   "fetch.unpackLimit=0",
                                   "-c",
                                   "pack.threads=1",
                                   "-c",
                                   "core.packedGitLimit=64m",
                                   "-c",
                                   "core.packedGitWindowSize=1m"});
    if (!network->certificateAuthorityFile.empty()) {
      const fs::path ca(network->certificateAuthorityFile);
      require(ca.is_absolute() && fs::canonical(ca) == ca &&
                  fs::is_regular_file(fs::symlink_status(ca)),
              "explicit TLS certificate authority must be a canonical regular file");
      command.insert(command.end(), {"-c", "http.sslCAInfo=" + ca.string()});
    }
  }
  command.insert(command.end(), arguments.begin(), arguments.end());
#ifdef _WIN32
  return detail::windowsGitProcess(gitExecutable, command, database.parent_path(), maximum, seconds,
                                   [&] {
                                     if (network)
                                       objectBudget(database, *network);
                                   });
#else
  std::vector<char *> argv;
  for (auto &item : command)
    argv.push_back(item.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment = {"PATH=/usr/bin:/bin",
                                          "HOME=/dev/null",
                                          "LC_ALL=C",
                                          "GIT_CONFIG_NOSYSTEM=1",
                                          "GIT_CONFIG_SYSTEM=/dev/null",
                                          "GIT_CONFIG_GLOBAL=/dev/null",
                                          "GIT_TERMINAL_PROMPT=0",
                                          "GIT_ASKPASS=/usr/bin/false",
                                          "GIT_ATTR_NOSYSTEM=1",
                                          "GIT_NO_LAZY_FETCH=1",
                                          "GIT_NO_REPLACE_OBJECTS=1",
                                          "GIT_OPTIONAL_LOCKS=0"};
  std::vector<char *> env;
  for (auto &item : environment)
    env.push_back(item.data());
  env.push_back(nullptr);
  int pipeFD[2];
  require(::pipe(pipeFD) == 0, "cannot create Git output pipe");
  if (::fcntl(pipeFD[0], F_SETFD, FD_CLOEXEC) == -1 ||
      ::fcntl(pipeFD[1], F_SETFD, FD_CLOEXEC) == -1) {
    ::close(pipeFD[0]);
    ::close(pipeFD[1]);
    throw std::runtime_error("cannot isolate Git output descriptors");
  }
  const auto pid = ::fork();
  if (pid == -1) {
    ::close(pipeFD[0]);
    ::close(pipeFD[1]);
    throw std::runtime_error("cannot start Git plumbing process");
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    if (network) {
      const rlimit limit{rlim_t(network->maxObjectBytes), rlim_t(network->maxObjectBytes)};
      if (::setrlimit(RLIMIT_FSIZE, &limit))
        ::_exit(126);
    }
    const auto null = ::open("/dev/null", O_RDWR);
    if (null == -1 || ::dup2(null, STDIN_FILENO) == -1 || ::dup2(null, STDERR_FILENO) == -1 ||
        ::dup2(pipeFD[1], STDOUT_FILENO) == -1)
      ::_exit(126);
    ::close(null);
    ::close(pipeFD[0]);
    ::close(pipeFD[1]);
    ::execve(argv[0], argv.data(), env.data());
    ::_exit(127);
  }
  ::close(pipeFD[1]);
  struct Child {
    pid_t pid;
    int fd;
    bool reaped = false;
    ~Child() {
      ::close(fd);
      ::kill(-pid, SIGKILL);
      if (!reaped) {
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }
    }
  } child{pid, pipeFD[0]};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::string result;
  bool eof = false, exited = false;
  int status = 0;
  while (!eof || !exited) {
    require(std::chrono::steady_clock::now() < deadline,
            "Git plumbing exceeded configured timeout");
    if (network)
      objectBudget(database, *network);
    if (!exited) {
      const auto waited = ::waitpid(pid, &status, WNOHANG);
      require(waited >= 0 || errno == EINTR, "cannot wait for Git plumbing");
      exited = waited == pid;
      if (exited)
        child.reaped = true;
    }
    if (!eof) {
      pollfd descriptor{pipeFD[0], POLLIN, 0};
      const auto ready = ::poll(&descriptor, 1, 10);
      require(ready >= 0 || errno == EINTR, "cannot poll Git output");
      if (ready > 0) {
        char buffer[16384];
        const auto count = ::read(pipeFD[0], buffer, sizeof(buffer));
        require(count >= 0 || errno == EINTR, "cannot read Git output");
        if (count > 0) {
          require(std::size_t(count) <= maximum - result.size(),
                  "Git output exceeds configured byte limit");
          result.append(buffer, std::size_t(count));
        } else if (count == 0)
          eof = true;
      }
    } else if (!exited)
      ::poll(nullptr, 0, 10);
  }
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          network
              ? "Git fetch failed (network, TLS, authentication, object budget, or unavailable "
                "pinned commit)"
              : "Git plumbing failed while reading the pinned commit (missing or invalid objects)");
  if (network)
    objectBudget(database, *network);
  return result;
#endif
}
std::string treeDigest(const fs::path &source, GitSnapshotLimits limits) {
  require(fs::is_directory(fs::symlink_status(source)), "snapshot source root must be a directory");
  std::vector<fs::path> entries;
  for (const auto &entry : fs::recursive_directory_iterator(source)) {
    const auto status = entry.symlink_status();
    require(fs::is_regular_file(status) || fs::is_directory(status),
            "snapshot contains a symlink or unsupported filesystem entry");
    require(entries.size() < limits.maxSourceEntries, "snapshot exceeds configured entry limit");
    entries.push_back(entry.path());
  }
  std::ranges::sort(entries);
  llvm::SHA256 hash;
  field(hash, "kalandra-source-tree-v1");
  std::size_t bytes = 0;
  for (const auto &path : entries) {
    field(hash, path.lexically_relative(source).generic_string());
    if (fs::is_directory(fs::symlink_status(path)))
      field(hash, "directory");
    else {
      field(hash, "file");
      auto contents = read(path, limits.maxSourceBytes - bytes);
      bytes += contents.size();
      field(hash, contents);
    }
  }
  return digest(hash);
}
void initializeDatabase(const fs::path &database) {
  fs::create_directories(database / "objects");
  fs::create_directory(database / "refs");
  write(database / "HEAD", "ref: refs/heads/unused\n");
  write(database / "config", "[core]\n bare = true\n repositoryformatversion = 0\n");
}
fs::path localGitDirectory(const fs::path &repository) {
  require(fs::canonical(repository).string() == repository.string(),
          "local Git repository must use its canonical absolute path");
  auto gitdir = repository;
  if (fs::exists(fs::symlink_status(repository / ".git")))
    gitdir /= ".git";
  require(fs::is_directory(fs::symlink_status(gitdir)) &&
              fs::is_directory(fs::symlink_status(gitdir / "objects")) &&
              fs::is_regular_file(fs::symlink_status(gitdir / "HEAD")),
          "snapshot requires a local worktree or bare Git repository; linked worktrees are "
          "unsupported");
  return gitdir;
}
void copyObjects(const fs::path &repository, const fs::path &database, GitSnapshotLimits limits) {
  const auto gitdir = localGitDirectory(repository);
  initializeDatabase(database);
  const auto objects = gitdir / "objects";
  std::size_t files = 0, bytes = 0;
  for (const auto &entry : fs::recursive_directory_iterator(objects)) {
    const auto status = entry.symlink_status();
    require(fs::is_regular_file(status) || fs::is_directory(status),
            "Git object store contains a symlink or unsupported entry");
    require(++files <= limits.maxObjectFiles, "Git object store exceeds configured entry limit");
    if (fs::is_directory(status))
      continue;
    const auto relative = entry.path().lexically_relative(objects);
    const auto parent = relative.parent_path().string();
    const auto name = relative.filename().string();
    require(relative != "info/alternates" && relative != "info/http-alternates" &&
                !name.ends_with(".promisor"),
            "alternate or partial Git object stores are unsupported");
    if (parent == "info")
      continue; // Never copy commit graphs or other source metadata.
    const bool loose = hex(parent, 2) && hex(name, 38);
    const auto extension = relative.extension().string();
    const bool pack = parent == "pack" && name.starts_with("pack-") &&
                      hex(name.substr(5, 40), 40) && name.size() == 45 + extension.size() &&
                      (extension == ".pack" || extension == ".idx" || extension == ".rev" ||
                       extension == ".bitmap");
    require(loose || pack, "unsupported Git object-store file");
    auto contents = read(entry.path(), limits.maxObjectBytes - bytes);
    bytes += contents.size();
    fs::create_directories((database / "objects" / relative).parent_path());
    write(database / "objects" / relative, contents);
  }
}
std::vector<std::string> revisionNames(const std::string &revision) {
  if (revision.starts_with("refs/"))
    return {revision};
  return {"refs/heads/" + revision, "refs/tags/" + revision};
}
std::string chooseRevision(const std::map<std::string, std::string> &refs,
                           const std::vector<std::string> &names, bool peeled) {
  std::string result;
  for (const auto &name : names) {
    auto found = refs.find(name);
    if (found == refs.end())
      continue;
    require(result.empty(), "ambiguous Git revision; use refs/heads/... or refs/tags/...");
    result = found->second;
    if (peeled) {
      if (auto tag = refs.find(name + "^{}"); tag != refs.end())
        result = tag->second;
    }
  }
  require(!result.empty(), "Git revision does not name an available branch or tag");
  return result;
}
std::string localRevision(const fs::path &repository, const fs::path &database,
                          const std::vector<std::string> &names, GitSnapshotLimits limits) {
  copyObjects(repository, database, limits);
  const auto source = localGitDirectory(repository);
  std::map<std::string, std::string> selected;
  if (fs::exists(fs::symlink_status(source / "packed-refs"))) {
    std::istringstream packed(read(source / "packed-refs", limits.maxListingBytes));
    std::string line;
    std::size_t count = 0;
    bool previous = false;
    while (std::getline(packed, line)) {
      require(count++ < limits.maxObjectFiles, "Git refs exceed configured entry limit");
      if (line.starts_with('#')) {
        previous = false;
        continue;
      }
      if (line.starts_with('^')) {
        require(previous && hex(std::string_view(line).substr(1), 40),
                "invalid peeled packed Git ref");
        previous = false;
        continue;
      }
      require(line.size() > 41 && line[40] == ' ' && hex(std::string_view(line).substr(0, 40), 40),
              "invalid packed Git ref record");
      const auto name = line.substr(41);
      if (std::ranges::find(names, name) != names.end())
        require(selected.emplace(name, line.substr(0, 40)).second, "duplicate packed Git ref");
      previous = true;
    }
  }
  for (const auto &name : names) {
    fs::path path = source;
    for (const auto &component : fs::path(name)) {
      path /= component;
      const auto status = fs::symlink_status(path);
      if (!fs::exists(status))
        break;
      if (path != source / name) {
        require(fs::is_directory(status), "Git ref paths must use real directories");
        continue;
      }
      auto value = read(path, 64);
      if (value.ends_with('\n'))
        value.pop_back();
      require(hex(value, 40), "local Git refs must contain exact object IDs, not symbolic refs");
      selected[name] = std::move(value); // A loose ref overrides its packed value.
    }
  }
  const auto object = chooseRevision(selected, names, false);
  const auto commit =
      git(database, {"rev-parse", "--verify", object + "^{commit}"}, 64, limits.processSeconds);
  require(commit.size() == 41 && commit.back() == '\n' &&
              hex(std::string_view(commit).substr(0, 40), 40),
          "Git revision does not identify a commit");
  return commit.substr(0, 40);
}
std::string remoteRevision(const std::string &repository, const fs::path &database,
                           const std::vector<std::string> &names, GitSnapshotLimits limits,
                           GitTransport transport) {
  initializeDatabase(database);
  std::vector<std::string> arguments{"ls-remote", "--", repository};
  auto expected = names;
  for (const auto &name : names) {
    arguments.push_back(name);
    if (name.starts_with("refs/tags/")) {
      arguments.push_back(name + "^{}");
      expected.push_back(name + "^{}");
    }
  }
  const auto advertisement = git(database, std::move(arguments), limits.maxListingBytes,
                                 limits.processSeconds, &limits, transport);
  std::map<std::string, std::string> selected;
  std::istringstream lines(advertisement);
  std::string line;
  while (std::getline(lines, line)) {
    require(line.size() > 41 && line[40] == '\t' && hex(std::string_view(line).substr(0, 40), 40),
            "invalid Git revision advertisement");
    const auto name = line.substr(41);
    require(std::ranges::find(expected, name) != expected.end() &&
                selected.emplace(name, line.substr(0, 40)).second,
            "unexpected or duplicate Git revision advertisement");
  }
  for (const auto &[name, object] : selected)
    if (name.ends_with("^{}"))
      require(selected.contains(name.substr(0, name.size() - 3)), "peeled Git ref lacks its tag");
  return chooseRevision(selected, names, true);
}
std::string extract(const fs::path &database, const std::string &commit, const fs::path &source,
                    GitSnapshotLimits limits) {
  require(git(database, {"cat-file", "-t", commit}, 32, limits.processSeconds) == "commit\n",
          "snapshot revision must identify a commit, not a tree or tag object");
  git(database, {"fsck", "--strict", "--no-reflogs", "--no-dangling", commit},
      limits.maxListingBytes, limits.processSeconds);
  auto listing = git(database, {"ls-tree", "-r", "-z", "--full-tree", commit},
                     limits.maxListingBytes, limits.processSeconds);
  fs::create_directory(source);
  std::size_t offset = 0, files = 0, bytes = 0;
  std::unordered_set<std::string> directories;
  while (offset < listing.size()) {
    auto end = listing.find('\0', offset);
    require(end != std::string::npos && ++files <= limits.maxSourceEntries,
            "Git tree exceeds configured file limit or has an invalid listing");
    const auto line = std::string_view(listing).substr(offset, end - offset);
    const auto tab = line.find('\t');
    require(tab == 52 && (line.starts_with("100644 blob ") || line.starts_with("100755 blob ")),
            "Git tree contains a symlink, submodule or unsupported mode");
    const std::string object(line.substr(12, 40));
    const fs::path relative(std::string(line.substr(tab + 1)));
    require(hex(object, 40) && !relative.empty() && !relative.has_root_path(),
            "invalid Git tree entry");
#ifdef _WIN32
    require(relative.generic_string() == line.substr(tab + 1),
            "Git source path is not a portable normalized path");
#endif
    for (const auto &part : relative) {
      auto name = part.string();
      std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
      });
      require(name != "." && name != ".." && name != ".git", "unsafe Git source path");
#ifdef _WIN32
      require(!name.empty() && name.back() != '.' && name.back() != ' ' &&
                  name.find_first_of("<>:\"\\|?*") == std::string::npos &&
                  std::ranges::none_of(name, [](unsigned char c) { return c < 32; }),
              "Git source path is unsafe on Windows");
      const auto stem = name.substr(0, name.find('.'));
      const auto suffix = stem.size() >= 3 ? stem.substr(3) : std::string{};
      const bool deviceNumber = (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
                                suffix == "¹" || suffix == "²" || suffix == "³";
      require(stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
                  stem != "conin$" && stem != "conout$" && stem != "clock$" &&
                  !((stem.starts_with("com") || stem.starts_with("lpt")) && deviceNumber),
              "Git source path uses a reserved Windows device name");
#endif
    }
    const auto target = source / relative;
    require(!fs::exists(target), "Git source paths collide on this filesystem");
    auto contents = git(database, {"cat-file", "blob", object}, limits.maxSourceBytes - bytes,
                        limits.processSeconds);
    bytes += contents.size();
    fs::path parent;
    for (const auto &part : relative.parent_path()) {
      parent /= part;
      if (fs::exists(source / parent))
        require(directories.contains(parent.generic_string()),
                "Git source directories collide on this filesystem");
      else {
        require(files + directories.size() < limits.maxSourceEntries,
                "Git source exceeds configured directory limit");
        fs::create_directory(source / parent);
        directories.insert(parent.generic_string());
      }
    }
    require(files + directories.size() <= limits.maxSourceEntries,
            "Git source exceeds configured entry limit");
    write(target, contents); // Source executable modes are intentionally stripped.
    offset = end + 1;
  }
  return treeDigest(source, limits);
}
NativeGitSnapshot load(const fs::path &cache, const GitSnapshotRequest &request,
                       GitSnapshotLimits limits) {
  const auto key = requestKey(request);
  const auto directory = cache / key;
  require(fs::is_directory(fs::symlink_status(directory)),
          "snapshot is missing or is not a directory");
  const auto table = toml::parse(read(directory / "snapshot.toml", 16384));
  require(table.size() == 4 && table["format"].is_integer() && table["format"].value<int>() == 1 &&
              table["repository"].value<std::string>() == request.repository &&
              table["commit"].value<std::string>() == request.commit,
          "snapshot metadata does not match request");
  auto expected = table["digest"].value<std::string>();
  require(expected && hex(*expected, 64), "invalid snapshot integrity digest");
  for (const auto &entry : fs::directory_iterator(directory))
    require(entry.path().filename() == "source" || entry.path().filename() == "snapshot.toml",
            "snapshot contains unexpected metadata entries");
  require(treeDigest(directory / "source", limits) == *expected,
          "snapshot source integrity mismatch");
  auto library = loadNativePackage((directory / "source/kalandra.toml").string());
  require(bool(library), library ? "" : library.error());
  const bool local = *gitTransport(request.repository) == GitTransport::Local;
  if (!local)
    for (const auto &dependency : library->dependencies)
      require(*gitTransport(dependency.repository) != GitTransport::Local,
              "remote libraries cannot declare local Git dependencies");
  return {(local ? "git-local-v1:" : "git-remote-v1:") + key, directory.string(), *expected,
          std::move(*library)};
}
} // namespace
std::expected<ResolvedGitSnapshot, std::string> installGitRevision(std::string cache,
                                                                   std::string repository,
                                                                   std::string revision,
                                                                   GitSnapshotLimits limits) {
  try {
    const auto transport = gitTransport(repository);
    require(bool(transport) && validGitRevision(revision),
            "invalid Git repository or revision request");
    std::string commit = revision;
    if (!exactGitCommit(revision)) {
      Stage stage(cacheRoot(cache));
      const auto names = revisionNames(revision);
      commit = *transport == GitTransport::Local
                   ? localRevision(repository, stage.path / "git", names, limits)
                   : remoteRevision(repository, stage.path / "git", names, limits, *transport);
    }
    auto snapshot = installGitSnapshot(std::move(cache), {std::move(repository), commit}, limits);
    require(bool(snapshot), snapshot ? "" : snapshot.error());
    return ResolvedGitSnapshot{commit, std::move(*snapshot)};
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::expected<NativeGitSnapshot, std::string>
loadGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits) {
  try {
    return load(cacheRoot(cache), request, limits);
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::expected<NativeGitSnapshot, std::string>
installGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits) {
  try {
    const auto root = cacheRoot(cache);
    const auto key = requestKey(request);
    const auto destination = root / key;
    if (fs::exists(fs::symlink_status(destination)))
      return load(root, request, limits);
    Stage stage(root);
    const auto transport = *gitTransport(request.repository);
    if (transport == GitTransport::Local)
      copyObjects(request.repository, stage.path / "git", limits);
    else {
      initializeDatabase(stage.path / "git");
      git(stage.path / "git",
          {"fetch", "--no-tags", "--no-write-fetch-head", "--no-recurse-submodules",
           "--no-auto-maintenance", "--depth=1", "--", request.repository, request.commit},
          limits.maxListingBytes, limits.processSeconds, &limits, transport);
    }
    fs::create_directory(stage.path / "published");
    const auto tree =
        extract(stage.path / "git", request.commit, stage.path / "published/source", limits);
    auto library = loadNativePackage((stage.path / "published/source/kalandra.toml").string());
    require(bool(library), library ? "" : library.error());
    if (transport != GitTransport::Local)
      for (const auto &dependency : library->dependencies)
        require(*gitTransport(dependency.repository) != GitTransport::Local,
                "remote libraries cannot declare local Git dependencies");
    const toml::table metadata{{"format", 1},
                               {"repository", request.repository},
                               {"commit", request.commit},
                               {"digest", tree}};
    std::ostringstream text;
    text << metadata;
    require(text.str().size() <= 16384, "snapshot metadata exceeds configured size limit");
    write(stage.path / "published/snapshot.toml", text.str());
    // Exclusive publication never replaces a competing or preexisting snapshot.
    const auto publication =
        detail::publishDirectoryExclusive(stage.path / "published", destination);
    if (publication) {
      require(publication == std::errc::file_exists ||
                  publication == std::errc::directory_not_empty || fs::exists(destination),
              "cannot publish snapshot without replacing an existing path");
      auto winner = load(root, request, limits);
      require(winner.digest == tree, "competing snapshot has different source contents");
      return winner;
    }
    return load(root, request, limits);
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
std::expected<NativeGitSnapshot, std::string>
loadLocalGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits) {
  const auto transport = gitTransport(request.repository);
  if (!transport || *transport != GitTransport::Local)
    return std::unexpected("local snapshot API requires a normalized absolute repository path");
  return loadGitSnapshot(std::move(cache), std::move(request), std::move(limits));
}
std::expected<NativeGitSnapshot, std::string>
installLocalGitSnapshot(std::string cache, GitSnapshotRequest request, GitSnapshotLimits limits) {
  const auto transport = gitTransport(request.repository);
  if (!transport || *transport != GitTransport::Local)
    return std::unexpected("local snapshot API requires a normalized absolute repository path");
  return installGitSnapshot(std::move(cache), std::move(request), std::move(limits));
}
} // namespace kalandra
