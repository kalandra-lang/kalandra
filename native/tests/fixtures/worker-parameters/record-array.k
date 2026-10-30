fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Job:
  run:fn(int)->int
fn forward(jobs:[Job])->[Job]:
  jobs
fn main()->int ! TaskError:
  jobs=try parallel:
    try await spawn forward([Job {run:double}])
  try parallel:
    try await spawn apply(21,jobs[0].run)
