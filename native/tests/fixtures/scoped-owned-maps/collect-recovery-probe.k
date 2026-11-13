error Stop:
  Done
fn checked(value:i64)->i64 ! Stop:
  return if value<0:
    fail Stop.Done
  else:
    value
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
fn divide(value:i64)->i64:
  return 6/value
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
    outcome=parallel:
      checkedValue=try checked(1)
      jobs=try array(for item in range(9):
        if item==8:
          ignored=try parallel:
            child=spawn divide(0)
            try await child
        yield make(item)
      )
      length(jobs)
    recovered=outcome catch fn(error):
      return match error:
        TaskError.Fault:0
        _:100
    total=total+value+recovered
    if index == faultAt:
      zero = 0
      total = total + 1 / zero
    index = index + 1
  return LoopResult {value: saved[0] + total + a[0] + length(a)}
fn main() -> i64:
  return 0
