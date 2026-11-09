type Job:
  run: fn(i64) -> i64
  data: [i64]
fn make(value: i64) -> Job:
  saved = [value, value + 1]
  return Job {run: fn(delta: i64): return saved[0] + delta, data: saved}
fn main() -> i64 ! TaskError:
  a = [1]
  a = [2]
  jobs = try parallelMap([], make)
  callbacks = try parallelMap([], fn(value: i64): return fn(x: i64): return x / value)
  return length(jobs) + length(callbacks) + a[0]
