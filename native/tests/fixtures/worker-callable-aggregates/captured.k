type Job:
  callbacks:[fn(i64)->i64]
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn make()->fn(i64)->i64:
  values=[40]
  job=Job {callbacks:[fn(value):return values[0]+value]}
  return fn(value:i64):return job.callbacks[0](value)
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make()
  return try parallel:
    try await spawn apply(2,f)
