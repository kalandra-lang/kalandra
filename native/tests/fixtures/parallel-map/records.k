type Job:
  value:i64
  run:fn(i64)->i64
fn main()->i64 ! TaskError:
  jobs=try parallelMap([40],fn(offset):return Job {value:2,run:fn(value:i64):return offset+value})
  values=try parallelMap(jobs,fn(job):return job.run(job.value))
  return values[0]
