fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Job:
  run:fn(i64)->i64
fn identity(job:Job)->Job:
  return job
fn make()->fn(i64)->i64:
  return double
fn main()->i64 ! TaskError:
  safe=identity(Job {run:double})
  maker=make
  other=identity(Job {run:maker()})
  return try parallel:
    try await spawn apply(21,safe.run)
