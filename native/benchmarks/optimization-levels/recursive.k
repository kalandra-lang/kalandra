from "std/parse" import parseI64, ParseError
from "std/io" import println, IoError
from "std/fmt" import format

fn tree(depth: i64, seed: i64) -> i64:
  if depth == 0:
    return seed % 97
  return tree(depth - 1, seed * 2) + tree(depth - 1, seed * 2 + 1)

fn main(args: [String]) -> unit ! (ParseError | IoError):
  depth = try parseI64(args[0])
  try println(format("{}", tree(depth, 1)))
  return ()
