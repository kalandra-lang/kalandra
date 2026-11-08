fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Job:
  run:fn(i64)->i64
fn forward(jobs:[Job])->[Job]:
  return jobs
fn main()->i64 ! TaskError:
  jobs=try parallel:
    try await spawn forward([Job {run:double}])
  return try parallel:
    try await spawn apply(21,jobs[0].run)
