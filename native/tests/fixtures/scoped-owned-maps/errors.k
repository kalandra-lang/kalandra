type Job:
  run: fn(int) -> int
error Stop:
  Bad:
    reason: String
    values: [int]
fn make(value: int) -> Job ! Stop:
  if value == 4:
    fail Stop.Bad {reason: "bad" + "!", values: [4]}
  Job {run: fn(x: int): value + x}
fn main() -> int:
  a = [1]
  a = [2]
  jobs = parallelMap(generate(9, fn(index): index), make) catch fn(error):
    match error:
      Stop.Bad {reason, values}: [Job {run: fn(x: int): length(reason) + values[0] + x}]
      TaskError.Fault: [Job {run: fn(x: int): 0}]
      TaskError.Cancelled: [Job {run: fn(x: int): 0}]
      TaskError.DeadlineExceeded: [Job {run: fn(x: int): 0}]
      TaskError.AdmissionRejected {reason}: [Job {run: fn(x: int): 0}]
  jobs[0].run(10) + a[0]
