from "std/gpu" import dispatch, WorkItem
from "std/io" import println, IoError
from "std/fmt" import format

fn prefix(row: String, initial: String) -> bool:
  return slice(row, 0, 1) == initial

fn inspect(rows: [String], initial: String) -> Tensor[i64, ?]:
  n = length(rows)
  return dispatch([n], [8], fn(item: WorkItem) -> i64:
    i = item.globalIndex(0)
    if i >= n:
      return 0
    row = rows[i]
    if byteLength(row) == 0:
      return 0
    scalar = row[0]
    same = if scalar == initial: 1
    else: 0
    also = if prefix(row, initial): 1
    else: 0
    return byteLength(row) * 1000 + length(row) * 100 + same * 10 + also
  )

fn main() -> unit ! (TaskError | IoError):
  rows = ["سé猫🧪", "", "س\0", "e\u{301}", "🧪"]
  result: Tensor[i64, ?] = try parallel:
    gpu = devices.gpu()
    counts = try await spawn inspect(rows, "س") on gpu
    try await transfer(counts, devices.cpu())
  values = array(result)
  try println(format("{} {} {} {} {}", values[0], values[1], values[2], values[3], values[4]))
  return ()
