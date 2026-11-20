#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ChildProcess.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>
#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif
#endif

namespace kalandra::runtime::process {
#ifndef _WIN32
namespace {
struct SpawnActions {
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  ~SpawnActions() {
    if (!error)
      posix_spawn_file_actions_destroy(&actions);
  }
};
struct SpawnAttributes {
  posix_spawnattr_t attributes;
  int error = posix_spawnattr_init(&attributes);
  ~SpawnAttributes() {
    if (!error)
      posix_spawnattr_destroy(&attributes);
  }
};
struct Ends {
  std::array<int, 6> files{-1, -1, -1, -1, -1, -1};
  ~Ends() {
    for (int fd : files)
      if (fd >= 0)
        ::close(fd);
  }
};
// Keep the pipe ends outside the stdio range even if the host closed fd 0, 1 or 2.
int makePipe(int *ends) {
#ifdef __APPLE__
  if (::pipe(ends))
    return errno;
#else
  if (::pipe2(ends, O_CLOEXEC))
    return errno;
#endif
  for (unsigned i = 0; i < 2; ++i) {
    if (ends[i] < 3) {
      int moved = fcntl(ends[i], F_DUPFD_CLOEXEC, 3);
      if (moved < 0)
        return errno;
      ::close(ends[i]);
      ends[i] = moved;
    }
#ifdef __APPLE__
    if (fcntl(ends[i], F_SETFD, FD_CLOEXEC) < 0)
      return errno;
#endif
  }
  return 0;
}
} // namespace
int launch(const Command &cmd, Child &child) {
  SpawnActions actions;
  SpawnAttributes attributes;
  if (actions.error || attributes.error)
    return actions.error ? actions.error : attributes.error;
  if (cmd.program.empty())
    return EINVAL;
  int error = 0;
  if (!cmd.directory.empty()) {
#ifdef __APPLE__
    error = posix_spawn_file_actions_addchdir(&actions.actions, cmd.directory.c_str());
#else
    error = posix_spawn_file_actions_addchdir_np(&actions.actions, cmd.directory.c_str());
#endif
    if (error)
      return error;
  }
  Ends ends;
  for (int stream = 0; stream < 3; ++stream) {
    if (cmd.streams[stream] == Stdio::Pipe) {
      auto *pair = ends.files.data() + 2 * stream;
      if ((error = makePipe(pair)))
        return error;
      const int parent = pair[stream == 0];
#ifdef __APPLE__
      if (stream == 0 && fcntl(parent, F_SETNOSIGPIPE, 1) < 0)
        return errno;
#endif
      const int flags = fcntl(parent, F_GETFL);
      if (flags < 0 || fcntl(parent, F_SETFL, flags | O_NONBLOCK) < 0)
        return errno;
      error = posix_spawn_file_actions_adddup2(&actions.actions, pair[stream != 0], stream);
    } else if (cmd.streams[stream] == Stdio::Null) {
      error = posix_spawn_file_actions_addopen(&actions.actions, stream, "/dev/null",
                                               stream == 0 ? O_RDONLY : O_WRONLY, 0);
    }
#ifdef __APPLE__
    else if (fcntl(stream, F_GETFD) >= 0)
      error = posix_spawn_file_actions_addinherit_np(&actions.actions, stream);
#endif
    if (error)
      return error;
  }
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
  if ((error = posix_spawn_file_actions_addclosefrom_np(&actions.actions, 3)))
    return error;
#endif
  sigset_t mask, defaults;
  sigemptyset(&mask);
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  if ((error = posix_spawnattr_setflags(&attributes.attributes, flags)) ||
      (error = posix_spawnattr_setsigmask(&attributes.attributes, &mask)) ||
      (error = posix_spawnattr_setsigdefault(&attributes.attributes, &defaults)))
    return error;
  std::vector<char *> args{const_cast<char *>(cmd.program.c_str())};
  for (const auto &s : cmd.arguments)
    args.push_back(const_cast<char *>(s.c_str()));
  args.push_back(nullptr);
#ifdef __APPLE__
  char **inherited = *_NSGetEnviron();
#else
  char **inherited = environ;
#endif
  std::vector<std::string> environment;
  for (auto p = inherited; p && *p; ++p)
    environment.emplace_back(*p);
  for (const auto &[name, value] : cmd.environment) {
    if (name.empty() || name.find('=') != std::string::npos)
      return EINVAL;
    const auto prefix = name + "=";
    std::erase_if(environment, [&](const auto &s) { return s.starts_with(prefix); });
    environment.push_back(prefix + value);
  }
  std::vector<char *> env;
  for (auto &pair : environment)
    env.push_back(pair.data());
  env.push_back(nullptr);
  pid_t pid;
  error = posix_spawnp(&pid, cmd.program.c_str(), &actions.actions, &attributes.attributes,
                       args.data(), env.data());
  if (error)
    return error;
  child.pid = pid;
  for (int i = 0; i < 3; ++i)
    if (cmd.streams[i] == Stdio::Pipe)
      child.pipes[i] = std::exchange(ends.files[2 * i + (i == 0)], -1);
  return 0;
}
int poll(Child &child) noexcept {
  if (child.finished)
    return 0;
  int status;
  int r;
  do {
    r = waitpid(child.pid, &status, WNOHANG);
  } while (r < 0 && errno == EINTR);
  if (r < 0)
    return errno;
  if (r) {
    child.finished = true;
    child.pid = -1;
    child.status.signaled = WIFSIGNALED(status);
    child.status.value = child.status.signaled ? WTERMSIG(status) : WEXITSTATUS(status);
  }
  return 0;
}
Child::~Child() {
  for (auto fd : pipes)
    if (fd >= 0)
      ::close(fd);
  if (pid > 0) {
    // waitpid first also detects hosts that configured automatic child reaping.
    if (!poll(*this) && !finished) {
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
}
#else
#include "WindowsChildProcess.inc"
#endif
} // namespace kalandra::runtime::process
