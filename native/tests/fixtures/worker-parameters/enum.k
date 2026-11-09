fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
enum Job:
  Empty
  Run:
    callback:fn(i64)->i64
fn forward(job:Job)->Job:
  return job
fn execute(job:Job)->i64:
  return match job:
    Job.Empty:0
    Job.Run {callback}:callback(21)
fn main()->i64 ! TaskError:
  job=try parallel:
    try await spawn forward(Job.Run {callback:double})
  return try parallel:
    try await spawn execute(job)
