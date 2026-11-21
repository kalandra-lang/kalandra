#pragma once
#include "BuildProgress.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include <array>
#include <filesystem>
#include <fstream>

namespace kalandra::cli {
// A child writing to the terminal can overwrite a live progress line. Keep its
// original diagnostics, then replay them above the status line when it returns.
inline int runBuildTool(BuildProgress &progress, llvm::StringRef program,
                        llvm::ArrayRef<llvm::StringRef> args,
                        std::optional<llvm::ArrayRef<llvm::StringRef>> env,
                        llvm::ArrayRef<std::optional<llvm::StringRef>> redirects, unsigned timeout,
                        std::string &error) {
  auto run = [&](llvm::ArrayRef<std::optional<llvm::StringRef>> io) {
    return llvm::sys::ExecuteAndWait(program, args, env, io, timeout, 0, &error);
  };
  if (!progress.live())
    return run(redirects);
  llvm::SmallString<256> path;
  if (llvm::sys::fs::createTemporaryFile("kalandra-tool", "log", path)) {
    progress.diagnostic();
    return run(redirects);
  }
  llvm::FileRemover remove(path);
  std::array<std::optional<llvm::StringRef>, 3> io{};
  std::copy(redirects.begin(), redirects.end(), io.begin());
  io[2] = path.str();
  const int result = run(io);
  std::ifstream messages(std::filesystem::path(std::u8string(path.begin(), path.end())),
                         std::ios::binary);
  if (messages.peek() != std::ifstream::traits_type::eof())
    progress.replay(messages);
  return result;
}
} // namespace kalandra::cli
