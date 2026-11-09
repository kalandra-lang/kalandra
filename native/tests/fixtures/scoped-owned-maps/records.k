type Job:
  run: fn(i64) -> i64
  data: [i64]
fn make(value: i64) -> Job:
  saved = [value, value + 1]
  return Job {run: fn(delta: i64): return saved[0] + delta, data: saved}
fn main() -> i64 ! TaskError:
  a = [1]
  a = [2]
  jobs = try parallelMap(generate(17, fn(index): return index), make)
  index = 0
  total = a[0]
  while index < length(jobs):
    total = total + jobs[index].run(jobs[index].data[1])
    index = index + 1
  return total
