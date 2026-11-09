type Job:
  run:fn(i64)->i64
  label:String
fn make()->[Job]:
  values=[20]
  return [Job {run:fn(value:i64):return values[0]+value,label:"hi"},Job {run:fn(value:i64):return values[0]-value,label:"hi"}]
fn main()->i64 ! TaskError:
  jobs=try parallel:
    try await spawn make()
  return jobs[0].run(1)+jobs[1].run(1)+length(jobs[0].label)
