fn double(value:int)->int:
  value*2
type Job:
  run:fn(int)->int
  values:[int]
fn make(value:int)->Job:
  numbers=[value]
  Job {run:fn(item:int):numbers[0]+item,values:[2]}
fn main()->int ! TaskError:
  factory=make
  job=try parallel:
    try await spawn factory(40)
  try parallel:
    try await spawn job.run(job.values[0])
