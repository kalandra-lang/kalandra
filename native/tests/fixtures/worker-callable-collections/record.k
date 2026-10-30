fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
type Job:
  run:fn(int)->int
  values:[int]
fn main()->int ! TaskError:
  jobs=[Job {run:make(40),values:[2]}]
  selected=try parallel:
    try await spawn slice(jobs,0,1)
  selected[0].run(selected[0].values[0])
