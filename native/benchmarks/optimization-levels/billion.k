from "std/parse" import parseI32, ParseError
from "std/io" import println, IoError
from "std/fmt" import format

fn row(count: i32, divisor: i32) -> i32:
  total: i32 = 0
  j: i32 = 0
  while j < count:
    total = total + j % divisor
    j = j + 1
  return total

fn main(args: [String]) -> unit ! (ParseError | IoError):
  divisor = try parseI32(args[0])
  offset = try parseI32(args[1])
  values: [i32] = generate(10000, fn(i): return row(100000, divisor) + offset)
  checksum: i64 = 0
  for value in values:
    checksum = checksum + i64(value)
  try println(format("{} {}", values[i64(offset)], checksum))
  return ()
