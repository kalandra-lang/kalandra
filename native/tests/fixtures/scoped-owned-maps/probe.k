type Job:
  run: fn(i64) -> i64
  data: [i64]
fn make(value: i64) -> Job:
  saved = [value, value + 1]
  return Job {run: fn(delta: i64): return saved[0] + delta, data: saved}
type LoopResult:
  value: i64
fn read(a: [i64]) -> i64:
  return a[0]
fn probe(n: i64, faultAt: i64) -> LoopResult ! TaskError:
  a = [7, 8]
  saved = a
  index = 0
  total = 0
  while index < n:
    value = try parallel:
      old = spawn read(a)
      a = if index % 2 == 0: [1, 2, 3]
      else: [4]
      try await old
    jobs = try parallelMap([0, 1, 2, 3, 4, 5, 6, 7, 8], make)
    total = total + value
    cell = 0
    while cell < length(jobs):
      total = total + jobs[cell].run(jobs[cell].data[1])
      cell = cell + 1
    if index == faultAt:
      zero = 0
      total = total + 1 / zero
    index = index + 1
  return LoopResult {value: saved[0] + total + a[0] + length(a)}
fn main() -> i64:
  return 0
