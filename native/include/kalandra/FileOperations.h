#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
namespace kalandra {
enum class FileAction : std::uint32_t {
  Open,
  BufferedReader,
  BufferedWriter,
  Read,
  ReadExact,
  ReadInto,
  ReadLine,
  Write,
  WriteAll,
  Flush,
  Finish,
  Close,
  SeekTo,
  WriteText,
  WriteLine,
  ReadIntoBuffer,
  ReadAppend,
  WriteBuffer,
  WriteAllBuffer,
  OpenDirectory,
  ReadDirectory,
  Metadata,
  CreateDirectory,
  RemoveFile,
  RemoveDirectory
};
inline std::optional<FileAction> fileAction(std::string_view name) {
  if (name == "__openDirectory")
    return FileAction::OpenDirectory;
  if (name == "__readDirectory")
    return FileAction::ReadDirectory;
  if (name == "metadata")
    return FileAction::Metadata;
  if (name == "createDir")
    return FileAction::CreateDirectory;
  if (name == "removeFile")
    return FileAction::RemoveFile;
  if (name == "removeDir")
    return FileAction::RemoveDirectory;
  if (name == "readAppend")
    return FileAction::ReadAppend;
  constexpr std::string_view names[] = {"open",      "bufferedReader", "bufferedWriter", "read",
                                        "readExact", "readInto",       "readLine",       "write",
                                        "writeAll",  "flush",          "finish",         "close",
                                        "seekTo",    "writeText",      "writeLine"};
  for (std::uint32_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
    if (name == names[i])
      return static_cast<FileAction>(i);
  return {};
}
inline bool filePathAction(FileAction action) {
  return action == FileAction::Open || action == FileAction::OpenDirectory ||
         action == FileAction::Metadata || action == FileAction::CreateDirectory ||
         action == FileAction::RemoveFile || action == FileAction::RemoveDirectory;
}
inline bool consumesFile(FileAction action) {
  return action == FileAction::BufferedReader || action == FileAction::BufferedWriter ||
         action == FileAction::Finish || action == FileAction::Close;
}
} // namespace kalandra
