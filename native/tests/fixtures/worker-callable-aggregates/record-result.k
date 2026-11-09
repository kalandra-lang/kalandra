type Job:
  run:fn(i64)->i64
  values:[i64]
fn make(offset:i64)->Job:
  values=[offset]
  return Job {run:fn(value:i64):return values[0]+value,values:[1]}
fn main()->i64 ! TaskError:
  job=try parallel:
    try await spawn make(40)
  return job.run(1)+job.values[0]
