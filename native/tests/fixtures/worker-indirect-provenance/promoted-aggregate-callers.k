fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Job:
  run:fn(int)->int
fn identity(job:Job)->Job:
  job
fn make()->fn(int)->int:
  double
fn main()->int ! TaskError:
  safe=identity(Job {run:double})
  maker=make
  other=identity(Job {run:maker()})
  try parallel:
    try await spawn apply(21,safe.run)
