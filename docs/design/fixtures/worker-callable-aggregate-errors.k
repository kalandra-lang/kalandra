error InputError:
  Invalid:
    values: [int]

type Job:
  callbacks: [fn(int) -> int ! InputError]

fn makeJob() -> Job:
  values = [20, 20]
  callback = fn(value: int) -> int ! InputError:
    if value == 0: fail InputError.Invalid {values: values}
    value
  Job {callbacks: [callback]}

fn main() -> int ! TaskError:
  job = try parallel:
    try await spawn makeJob()
  job.callbacks[0](0) catch fn(error):
    match error:
      InputError.Invalid {values}: values[0] + values[1] + 2
