fn double(value:i64)->i64:
  return value*2
type Job:
  run:fn(i64)->i64
fn main()->i64 ! TaskError:
  job=Job {run:double}
  return try parallel:
    try await spawn job.run(21)
