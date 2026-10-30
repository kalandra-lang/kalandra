type Job:
  run:fn(int)->int
  values:[int]
error E:
  Retry:
    job:Job
fn failer()->int ! E:
  numbers=[40]
  fail E.Retry {job:Job {run:fn(value:int):numbers[0]+value,values:[2]}}
fn run()->int ! (E | TaskError):
  try parallel:
    try await spawn failer()
fn main()->int ! TaskError:
  run() catch fn(error):
    match error:
      E.Retry {job}:job.run(job.values[0])
      _:fail error
