from "std/process" import command, capture, start, wait, takeStdin, takeStdout, takeStderr, withStdin, withStdout, withStderr, withDirectory, withEnv, Stdio, ProcessStatus, ProcessError, close as closePipe
from "std/stream" import ByteReader, ByteWriter, readInto, writeAll, StreamError
from "std/buffer" import ByteBuffer, byteBuffer, BufferError
from "std/fs" import open, OpenMode, close, FsError
from "std/fmt" import format
from "std/io" import println, IoError

fn code(status: ProcessStatus) -> i64:
  return match status:
    ProcessStatus.Exited {code}: i64(code)
    ProcessStatus.Signaled {signal}: 0 - i64(signal)

fn pump[R: ByteReader, W: ByteWriter](source: R, target: W, mut buffer: ByteBuffer) -> unit ! StreamError:
  while true:
    count = try readInto(source, buffer, 65536)
    if count == 0:
      break
    try writeAll(target, buffer)
  return ()

fn main(args: [String]) -> unit ! (ProcessError | StreamError | BufferError | FsError | IoError):
  program = args[0]
  cmd = command(program, ["helper.py", "both", "", "a b", "x$()", "a\"b"])
  configured = withDirectory(withEnv(cmd, "KALANDRA_STREAM_TEST", "child value"), "child")
  captured = try capture(configured, 140002)
  try println(format("{}:{}:{}:{}:{}", code(captured.status), length(captured.stdout), length(captured.stderr), captured.stdout[70000], captured.stderr[70000]))

  source = try open("input.bin", OpenMode.Read)
  sink = try open("result.bin", OpenMode.Create)
  chunk = try byteBuffer(65536)
  importer = try start(withStdin(command(program, ["child/helper.py", "store"]), Stdio.Pipe))
  input = try takeStdin(importer)
  try pump(source, input, chunk)
  try closePipe(input)
  try close(source)
  first = try wait(importer)
  second = try wait(importer)
  try println(format("{}:{}", code(first), code(second)))
  exporter = try start(withStdout(command(program, ["child/helper.py", "emit"]), Stdio.Pipe))
  output = try takeStdout(exporter)
  try pump(output, sink, chunk)
  try closePipe(output)
  try close(sink)
  finished = try wait(exporter)
  try println(format("{}", code(finished)))
  return ()
