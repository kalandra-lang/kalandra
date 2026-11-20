from "std/process" import command, capture, ProcessError
from "std/io" import println, IoError
from "std/fmt" import format

fn twice(n: i64) -> i64:
  return n * 2

fn main(args: [String]) -> unit ! (IoError | ProcessError | TaskError):
  child = try capture(command(args[0], ["-c", "import sys;sys.stdout.buffer.write(bytes([21]))"]), 1)
  value = i64(child.stdout[0])
  doubled = try parallel:
    gpu = devices.gpu()
    try await spawn twice(value) on gpu
  try println(format("{}", doubled))
  return ()
