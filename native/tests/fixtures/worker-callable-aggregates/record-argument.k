type Job:
  run:fn(i64)->i64
  label:String
fn apply(job:Job)->i64:
  return job.run(2)+length(job.label)
fn main()->i64 ! TaskError:
  values=[38]
  f=fn(value:i64):return values[0]+value
  return try parallel:
    try await spawn apply(Job {run:f,label:"hi"})
