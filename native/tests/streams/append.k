from "std/process" import command, start, wait, withStdout, Stdio, takeStdout, ProcessError
from "std/stream" import readAppend, write, close, StreamError
from "std/buffer" import byteBuffer, BufferError, append
from "std/fs" import open, close as closeFile, FsError, OpenMode
from "std/io" import println, IoError
from "std/fmt" import format

fn main(args: [String]) -> unit ! (ProcessError | StreamError | BufferError | FsError | IoError):
  cmd = command(args[0], ["-c", "import sys; sys.stdout.buffer.write(bytes([0, 255]))"])
  child = try start(withStdout(cmd, Stdio.Pipe))
  pipe = try takeStdout(child)
  finished = try wait(child)
  data = try byteBuffer(3)
  try append(data, [42])
  while true:
    count = try readAppend(pipe, data, 1)
    if count == 0:
      break
  try close(pipe)
  file = try open("append.bin", OpenMode.Create)
  written = try write(file, data)
  try closeFile(file)
  try println(format("{}", written))
  return ()
