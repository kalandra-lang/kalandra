type Job:
  run:fn(int)->int
  values:[int]
fn make(offset:int)->Job:
  values=[offset]
  Job {run:fn(value:int):values[0]+value,values:[1]}
fn main()->int ! TaskError:
  job=try parallel:
    try await spawn make(40)
  job.run(1)+job.values[0]
