error InputError:
  Invalid:
    values: [i64]

type Job:
  callbacks: [fn(i64) -> i64 ! InputError]

fn makeJob() -> Job:
  values = [20, 20]
  callback = fn(value: i64) -> i64 ! InputError:
    if value == 0: fail InputError.Invalid {values: values}
    return value
  return Job {callbacks: [callback]}

fn main() -> i64 ! TaskError:
  job = try parallel:
    try await spawn makeJob()
  return job.callbacks[0](0) catch fn(error):
    return match error:
      InputError.Invalid {values}: values[0] + values[1] + 2
