type Job:
  run: fn(i64) -> i64
  data: [i64]
fn make(value: i64) -> Job:
  saved = [value, value + 1]
  return Job {run: fn(delta: i64): return saved[0] + delta, data: saved}
fn main() -> i64 ! TaskError:
  a = [1]
  index = 0
  total = 0
  while index < 100:
    jobs = try parallelMap(generate(index % 17, fn(value): return value), make)
    cell = 0
    while cell < length(jobs):
      total = total + jobs[cell].run(jobs[cell].data[1])
      cell = cell + 1
    a = [index + 1]
    index = index + 1
  return total + a[0]
