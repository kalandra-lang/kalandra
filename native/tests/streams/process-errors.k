from "std/process" import command, capture, start, wait, takeStdin, withStdin, Stdio, ProcessStatus, ProcessError, Child, close
from "std/stream" import StreamError, writeAll
from "std/buffer" import byteBuffer, append, BufferError
from "std/io" import println, IoError
from "std/fmt" import format

fn missing() -> i64 ! ProcessError:
  child = try start(command("kalandra-321-missing-executable", []))
  return 0

fn overflow(program: String) -> i64 ! ProcessError:
  result = try capture(command(program, ["child/helper.py", "limit"]), 4)
  return 0

fn repeatTake(child: Child) -> i64 ! ProcessError:
  input = try takeStdin(child)
  return 0

fn broken(child: Child) -> i64 ! (ProcessError | BufferError | StreamError):
  writer = try takeStdin(child)
  data = try byteBuffer(1)
  try append(data, [42])
  try writeAll(writer, data)
  return 0

fn main(args: [String]) -> unit ! (ProcessError | StreamError | IoError | BufferError):
  missingCode = missing() catch fn(e): return match e:
    ProcessError.System {code, message}: i64(code)
    _: 0
  overflowLimit = overflow(args[0]) catch fn(e): return match e:
    ProcessError.OutputLimit {limit}: i64(limit)
    _: 0
  child = try start(withStdin(command(args[0], ["child/helper.py", "empty"]), Stdio.Pipe))
  status = try wait(child)
  pipeError = broken(child) catch fn(e): return match e:
    StreamError.BrokenPipe {code}: i64(code)
    _: 0
  duplicate = repeatTake(child) catch fn(e): return match e:
    ProcessError.PipeUnavailable: 1
    _: 0
  empty = try capture(command(args[0], ["child/helper.py", "empty"]), 0)
  try println(format("{}:{}:{}:{}:{}:{}", missingCode != 0, overflowLimit, pipeError != 0, duplicate, length(empty.stdout), length(empty.stderr)))
  stopped = try capture(command(args[0], ["child/helper.py", "status"]), 0)
  numeric = match stopped.status:
    ProcessStatus.Exited {code}: i64(code)
    ProcessStatus.Signaled {signal}: 0 - i64(signal)
  try println(format("{}", numeric))
  return ()
