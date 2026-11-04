type Job:
  run: fn(int) -> int
  data: [int]
fn make(value: int) -> Job:
  saved = [value, value + 1]
  Job {run: fn(delta: int): saved[0] + delta, data: saved}
fn main() -> int ! TaskError:
  a = [1]
  a = [2]
  jobs = try parallelMap(generate(17, fn(index): index), make)
  index = 0
  total = a[0]
  while index < length(jobs):
    total = total + jobs[index].run(jobs[index].data[1])
    index = index + 1
  total
