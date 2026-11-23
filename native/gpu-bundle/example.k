from "std/io" import println, IoError
from "std/fmt" import format

fn twice(x: i32) -> i32:
  return x * 2

fn main(args: [String]) -> unit ! (TaskError | IoError):
  if length(args) == 0:
    try println("cpu")
    return ()
  result = try parallel:
    gpu = devices.gpu()
    try await spawn twice(21) on gpu
  try println(format("{}", result))
  return ()
