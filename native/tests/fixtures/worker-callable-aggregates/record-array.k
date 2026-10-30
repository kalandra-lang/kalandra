type Job:
  run:fn(int)->int
  label:String
fn make()->[Job]:
  values=[20]
  [Job {run:fn(value:int):values[0]+value,label:"hi"},Job {run:fn(value:int):values[0]-value,label:"hi"}]
fn main()->int ! TaskError:
  jobs=try parallel:
    try await spawn make()
  jobs[0].run(1)+jobs[1].run(1)+length(jobs[0].label)
