type Job:
  run: fn(i64) -> i64
error Stop:
  Bad:
    reason: String
    values: [i64]
fn make(value: i64) -> Job ! Stop:
  if value == 4:
    fail Stop.Bad {reason: "bad" + "!", values: [4]}
  return Job {run: fn(x: i64): return value + x}
fn main() -> i64:
  a = [1]
  a = [2]
  jobs = parallelMap(generate(9, fn(index): return index), make) catch fn(error):
    return match error:
      Stop.Bad {reason, values}: [Job {run: fn(x: i64): return length(reason) + values[0] + x}]
      TaskError.Fault: [Job {run: fn(x: i64): return 0}]
      TaskError.Cancelled: [Job {run: fn(x: i64): return 0}]
      TaskError.DeadlineExceeded: [Job {run: fn(x: i64): return 0}]
      TaskError.AdmissionRejected {reason}: [Job {run: fn(x: i64): return 0}]
  return jobs[0].run(10) + a[0]
