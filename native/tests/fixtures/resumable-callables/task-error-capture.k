type Outcome:
  error:TaskError
fn identity(value:i64)->i64:
  return value
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn identity(value)
fn capture(error:TaskError)->(fn(i64)->i64 ! TaskError):
  outcome=Outcome {error:error}
  return fn(value:i64)->i64 ! TaskError:
    selected=match outcome.error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:value
          _:0
      _:0
    return try parallel:
      try await spawn identity(selected)
fn failer(divisor:i64)->(fn(i64)->i64 ! TaskError):
  ignored=1/divisor
  return run
fn factory()->(fn(i64)->i64 ! TaskError) ! TaskError:
  return try parallel:
    try await spawn failer(0)
fn main()->i64 ! TaskError:
  callback=factory() catch fn(error):return capture(error)
  return try callback(42)
