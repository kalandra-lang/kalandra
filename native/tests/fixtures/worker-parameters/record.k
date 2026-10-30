fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
type Job:
  run:fn(int)->int
  data:[int]
fn forward(job:Job)->Job:
  job
fn execute(job:Job)->int ! TaskError:
  try parallel:
    try await spawn apply(job.data[0],job.run)
fn main()->int ! TaskError:
  job=try parallel:
    try await spawn forward(Job {run:double,data:[21]})
  try parallel:
    try await spawn execute(job)
