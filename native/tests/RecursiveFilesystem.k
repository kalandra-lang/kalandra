from "std/fs" import walkDir, createDirAll, removeTree, FsError, FileKind, watchDir, WatchScope
from "std/io" import println, IoError
from "std/fmt" import format

fn main(args: [String]) -> unit ! (FsError | IoError):
  path = args[1]
  if args[0] == "watch":
    watcher = try watchDir(path, WatchScope.Recursive)
    return ()
  if args[0] == "create":
    try createDirAll(path)
    return ()
  if args[0] == "remove":
    try removeTree(path)
    return ()
  try for entry in try walkDir(path):
    kind = match entry.kind:
      FileKind.File: "file"
      FileKind.Directory: "dir"
      FileKind.Symlink: "link"
      FileKind.Other: "other"
    try println(format("{} {}", kind, entry.path))
    if args[0] == "first":
      break
  return ()
