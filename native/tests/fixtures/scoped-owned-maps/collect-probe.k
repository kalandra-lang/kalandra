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
    jobs = for value in range(9):
      yield make(value)
    total = total + value
    cell = 0
    while cell < length(jobs):
      total = total + jobs[cell].run(jobs[cell].data[1])
      cell = cell + 1
    if index == faultAt:
      zero = 0
      total = total + 1 / zero
    index = index + 1
  LoopResult {value: saved[0] + total + a[0] + length(a)}
fn main() -> int:
  0
