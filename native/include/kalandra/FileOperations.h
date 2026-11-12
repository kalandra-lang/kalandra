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
  WriteAllBuffer
};
inline std::optional<FileAction> fileAction(std::string_view name) {
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
inline bool consumesFile(FileAction action) {
  return action == FileAction::BufferedReader || action == FileAction::BufferedWriter ||
         action == FileAction::Finish || action == FileAction::Close;
}
} // namespace kalandra
