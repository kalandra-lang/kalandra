type Job:
  run:fn(i64)->i64
  values:[i64]
error E:
  Retry:
    job:Job
fn failer()->i64 ! E:
  numbers=[40]
  fail E.Retry {job:Job {run:fn(value:i64):return numbers[0]+value,values:[2]}}
fn run()->i64 ! (E | TaskError):
  return try parallel:
    try await spawn failer()
fn main()->i64 ! TaskError:
  return run() catch fn(error):
    return match error:
      E.Retry {job}:job.run(job.values[0])
      _:fail error
