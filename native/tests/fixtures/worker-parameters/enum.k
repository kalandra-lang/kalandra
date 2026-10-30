fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
enum Job:
  Empty
  Run:
    callback:fn(int)->int
fn forward(job:Job)->Job:
  job
fn execute(job:Job)->int:
  match job:
    Job.Empty:0
    Job.Run {callback}:callback(21)
fn main()->int ! TaskError:
  job=try parallel:
    try await spawn forward(Job.Run {callback:double})
  try parallel:
    try await spawn execute(job)
