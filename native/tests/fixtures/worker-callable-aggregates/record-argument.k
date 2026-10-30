type Job:
  run:fn(int)->int
  label:String
fn apply(job:Job)->int:
  job.run(2)+length(job.label)
fn main()->int ! TaskError:
  values=[38]
  f=fn(value:int):values[0]+value
  try parallel:
    try await spawn apply(Job {run:f,label:"hi"})
