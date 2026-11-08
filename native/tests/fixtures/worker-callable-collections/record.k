fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
type Job:
  run:fn(i64)->i64
  values:[i64]
fn main()->i64 ! TaskError:
  jobs=[Job {run:make(40),values:[2]}]
  selected=try parallel:
    try await spawn slice(jobs,0,1)
  return selected[0].run(selected[0].values[0])
