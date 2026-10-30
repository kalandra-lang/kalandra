type Job:
  run:fn(int)->int
  values:[int]
fn main()->int ! TaskError:
  jobs=try parallel:
    try await spawn map([40],fn(offset):Job {run:fn(value:int):offset+value,values:[2]})
  jobs[0].run(jobs[0].values[0])
