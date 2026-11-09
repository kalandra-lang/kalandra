fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Job:
  run:fn(i64)->i64
  data:[i64]
fn forward(job:Job)->Job:
  return job
fn execute(job:Job)->i64 ! TaskError:
  return try parallel:
    try await spawn apply(job.data[0],job.run)
fn main()->i64 ! TaskError:
  job=try parallel:
    try await spawn forward(Job {run:double,data:[21]})
  return try parallel:
    try await spawn execute(job)
