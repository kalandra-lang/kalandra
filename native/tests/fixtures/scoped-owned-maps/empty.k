type Job:
  run: fn(int) -> int
  data: [int]
fn make(value: int) -> Job:
  saved = [value, value + 1]
  Job {run: fn(delta: int): saved[0] + delta, data: saved}
fn main() -> int ! TaskError:
  a = [1]
  a = [2]
  jobs = try parallelMap([], make)
  callbacks = try parallelMap([], fn(value: int): fn(x: int): x / value)
  length(jobs) + length(callbacks) + a[0]
