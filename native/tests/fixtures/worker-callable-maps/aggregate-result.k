type Job:
  run:fn(i64)->i64
  values:[i64]
fn main()->i64 ! TaskError:
  jobs=try parallel:
    try await spawn map([40],fn(offset):return Job {run:fn(value:i64):return offset+value,values:[2]})
  return jobs[0].run(jobs[0].values[0])
