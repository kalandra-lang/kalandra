type Job:
  callbacks:[fn(int)->int]
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn make()->fn(int)->int:
  values=[40]
  job=Job {callbacks:[fn(value):values[0]+value]}
  fn(value:int):job.callbacks[0](value)
fn main()->int ! TaskError:
  f=try parallel:
    try await spawn make()
  try parallel:
    try await spawn apply(2,f)
