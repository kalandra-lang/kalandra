from "std/parse" import parseI64, ParseError
from "std/io" import println, IoError
from "std/fmt" import format

fn sumSquares(count: i64) -> f64:
  total: f64 = 0.0
  for i in range(count):
    x = f64(i % 257) * 0.125
    total = total + x * x
  return total

fn main(args: [String]) -> unit ! (ParseError | IoError):
  count = try parseI64(args[0])
  try println(format("{}", i64(sumSquares(count) * 64.0)))
  return ()
