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
  DirectoryCreate,
  RemoveFile,
  DirectoryRemove,
  StreamRead,
  StreamAppend,
  StreamWrite,
  StreamWriteAll,
  PipeClose,
  Start,
  TakeStdin,
  TakeStdout,
  TakeStderr,
  Wait,
  Capture,
  OpenWalk,
  DirectoryCreateAll,
  RemoveTree,
  OpenWatch,
  ReadWatch
};
inline std::optional<FileAction> fileAction(std::string_view name) {
  if (name == "__openWatch")
    return FileAction::OpenWatch;
  if (name == "__readWatch")
    return FileAction::ReadWatch;
  if (name == "__openWalk")
    return FileAction::OpenWalk;
  if (name == "createDirAll")
    return FileAction::DirectoryCreateAll;
  if (name == "removeTree")
    return FileAction::RemoveTree;
  if (name == "__openDirectory")
    return FileAction::OpenDirectory;
  if (name == "__readDirectory")
    return FileAction::ReadDirectory;
  if (name == "metadata")
    return FileAction::Metadata;
  if (name == "createDir")
    return FileAction::DirectoryCreate;
  if (name == "removeFile")
    return FileAction::RemoveFile;
  if (name == "removeDir")
    return FileAction::DirectoryRemove;
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
inline std::optional<FileAction> streamAction(std::string_view name) {
  if (name == "__readIntoFile")
    return FileAction::StreamRead;
  if (name == "__readAppendFile")
    return FileAction::StreamAppend;
  if (name == "__readIntoBufferedReader")
    return FileAction::StreamRead;
  if (name == "__readAppendBufferedReader")
    return FileAction::StreamAppend;
  if (name == "__readIntoPipeReader")
    return FileAction::StreamRead;
  if (name == "__readAppendPipeReader")
    return FileAction::StreamAppend;
  if (name == "__writeFile")
    return FileAction::StreamWrite;
  if (name == "__writeAllFile")
    return FileAction::StreamWriteAll;
  if (name == "__writeBufferedWriter")
    return FileAction::StreamWrite;
  if (name == "__writeAllBufferedWriter")
    return FileAction::StreamWriteAll;
  if (name == "__writePipeWriter")
    return FileAction::StreamWrite;
  if (name == "__writeAllPipeWriter")
    return FileAction::StreamWriteAll;
  if (name == "__closeReader")
    return FileAction::PipeClose;
  if (name == "__closeWriter")
    return FileAction::PipeClose;
  return {};
}
inline std::optional<FileAction> processAction(std::string_view name) {
  if (name == "start")
    return FileAction::Start;
  if (name == "takeStdin")
    return FileAction::TakeStdin;
  if (name == "takeStdout")
    return FileAction::TakeStdout;
  if (name == "takeStderr")
    return FileAction::TakeStderr;
  if (name == "wait")
    return FileAction::Wait;
  if (name == "capture")
    return FileAction::Capture;
  return {};
}
inline bool isProcessAction(FileAction action) {
  return action >= FileAction::Start && action <= FileAction::Capture;
}
inline bool commandAction(FileAction action) {
  return action == FileAction::Start || action == FileAction::Capture;
}
inline FileAction streamFileAction(FileAction action) {
  switch (action) {
  case FileAction::StreamRead:
    return FileAction::ReadIntoBuffer;
  case FileAction::StreamAppend:
    return FileAction::ReadAppend;
  case FileAction::StreamWrite:
    return FileAction::WriteBuffer;
  case FileAction::StreamWriteAll:
    return FileAction::WriteAllBuffer;
  case FileAction::PipeClose:
    return FileAction::Close;
  default:
    return action;
  }
}
inline bool filePathAction(FileAction action) {
  return action == FileAction::OpenWatch || action == FileAction::OpenWalk || action == FileAction::DirectoryCreateAll ||
         action == FileAction::RemoveTree || action == FileAction::Open ||
         action == FileAction::OpenDirectory || action == FileAction::Metadata ||
         action == FileAction::DirectoryCreate || action == FileAction::RemoveFile ||
         action == FileAction::DirectoryRemove;
}
inline bool consumesFile(FileAction action) {
  return action == FileAction::PipeClose || action == FileAction::BufferedReader ||
         action == FileAction::BufferedWriter || action == FileAction::Finish ||
         action == FileAction::Close;
}
} // namespace kalandra
