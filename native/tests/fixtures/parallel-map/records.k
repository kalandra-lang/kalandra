type Job:
  value:int
  run:fn(int)->int
fn main()->int ! TaskError:
  jobs=try parallelMap([40],fn(offset):Job {value:2,run:fn(value:int):offset+value})
  values=try parallelMap(jobs,fn(job):job.run(job.value))
  values[0]
