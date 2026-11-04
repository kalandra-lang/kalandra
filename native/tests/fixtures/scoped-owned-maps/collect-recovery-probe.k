error Stop:
  Done
fn checked(value:int)->int ! Stop:
  if value<0:
    fail Stop.Done
  else:
    value
type Job:
  run: fn(int) -> int
  data: [int]
fn make(value: int) -> Job:
  saved = [value, value + 1]
  Job {run: fn(delta: int): saved[0] + delta, data: saved}
type LoopResult:
  value: int
fn read(a: [int]) -> int:
  a[0]
fn divide(value:int)->int:
  6/value
fn probe(n: int, faultAt: int) -> LoopResult ! TaskError:
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
      jobs=for item in range(9):
        if item==8:
          child=spawn divide(0)
          ignored=try await child
        yield make(item)
      length(jobs)
    recovered=outcome catch fn(error):
      match error:
        TaskError.Fault:0
        _:100
    total=total+value+recovered
    if index == faultAt:
      zero = 0
      total = total + 1 / zero
    index = index + 1
  LoopResult {value: saved[0] + total + a[0] + length(a)}
fn main() -> int:
  0
