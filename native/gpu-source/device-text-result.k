from "std/io" import println, IoError

fn make(rows: [String], delimiter: String, index: i64) -> String:
  row = rows[index]
  firstPart = slice(row, 1, length(row))
  if firstPart != "é猫🧪":
    return "wrong"
  return row[0] + delimiter + firstPart

fn identity(text: String) -> String:
  return text

fn main(args: [String]) -> unit ! (TaskError | IoError):
  index = if length(args) == 0: 1
  else: -1
  text = try parallel:
    value = try await spawn make(["discard", "سé猫🧪"], "\0/", index) on devices.gpu()
    try await spawn identity(value) on devices.gpu()
  try println(text)
  return ()
