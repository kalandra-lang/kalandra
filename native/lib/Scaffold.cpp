#include "PlatformFileSystem.h"
#include "kalandra/Project.h"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace kalandra {
std::expected<void, std::string> createNativeProject(std::string path) {
  namespace fs = std::filesystem;
  if (path.empty() || path.find('\0') != std::string::npos)
    return std::unexpected("new project requires a nonempty directory path");
  std::error_code error;
  auto destination = fs::absolute(path, error);
  if (!error && destination.filename().empty() && destination != destination.root_path())
    destination = destination.parent_path();
  if (error || destination.filename().empty())
    return std::unexpected("new project requires a new directory name");
  const auto parent = fs::canonical(destination.parent_path(), error);
  if (error || !fs::is_directory(parent, error))
    return std::unexpected("new project parent directory must exist");
  destination = parent / destination.filename();
  const auto status = fs::symlink_status(destination, error);
  if (error && error != std::errc::no_such_file_or_directory)
    return std::unexpected("cannot inspect new project destination: " + error.message());
  if (fs::exists(status) || fs::is_symlink(status))
    return std::unexpected("new project destination already exists: " + destination.string());
  fs::path staging;
  if (auto error = detail::createPrivateStage(parent, ".kalandra-new-", staging))
    return std::unexpected("cannot stage new project: " + error.message());
  struct Stage {
    fs::path path;
    ~Stage() {
      if (!path.empty()) {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    }
  } stage{staging};
  if (!fs::create_directory(stage.path / "src", error))
    return std::unexpected("cannot create starter source directory: " + error.message());
  const auto write = [&](const char *name, std::string_view text) {
    std::ofstream output(stage.path / name, std::ios::binary);
    output << text;
    output.close();
    return bool(output);
  };
  if (!write("kalandra.toml", "format = 1\n\n[project]\nentry = \"src/main.k\"\n") ||
      !write("src/math.k", "export fn double(value: int) -> int:\n  value * 2\n") ||
      !write("src/main.k", "from \"./math.k\" import double\n\nfn main() -> [int] ! TaskError:\n  "
                           "try parallelMap([1, 2, 3], double)\n"))
    return std::unexpected("cannot write native starter files");
  // Exclusive rename publishes the complete project atomically and
  // rejects a destination created after the initial check, including empty dirs.
  if (auto publication = detail::publishDirectoryExclusive(stage.path, destination))
    return std::unexpected("cannot publish new project without replacing an existing path: " +
                           publication.message());
  stage.path.clear();
  return {};
}
} // namespace kalandra
