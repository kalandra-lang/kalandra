fn double(value:i64)->i64:
  return value*2
type Job:
  run:fn(i64)->i64
  values:[i64]
fn make(value:i64)->Job:
  numbers=[value]
  return Job {run:fn(item:i64):return numbers[0]+item,values:[2]}
fn main()->i64 ! TaskError:
  factory=make
  job=try parallel:
    try await spawn factory(40)
  return try parallel:
    try await spawn job.run(job.values[0])
