type Job:
  run: fn(int) -> int
  data: [int]
fn make(value: int) -> Job:
  saved = [value, value + 1]
  Job {run: fn(delta: int): saved[0] + delta, data: saved}
fn main() -> int ! TaskError:
  a = [1]
  index = 0
  total = 0
  while index < 100:
    jobs = try parallelMap(generate(index % 17, fn(value): value), make)
    cell = 0
    while cell < length(jobs):
      total = total + jobs[cell].run(jobs[cell].data[1])
      cell = cell + 1
    a = [index + 1]
    index = index + 1
  total + a[0]
