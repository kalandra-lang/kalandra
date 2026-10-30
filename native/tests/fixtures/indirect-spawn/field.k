fn double(value:int)->int:
  value*2
type Job:
  run:fn(int)->int
fn main()->int ! TaskError:
  job=Job {run:double}
  try parallel:
    try await spawn job.run(21)
