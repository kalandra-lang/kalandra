type Outcome:
  error:TaskError
fn divide(value:i64)->i64:
  return 1/value
fn start()->i64 ! TaskError:
  return try parallel:
    try await spawn divide(0)
fn callback(error:TaskError)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return match outcome.error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:value
          _:0
      _:0
fn recovered(error:TaskError)->i64 ! TaskError:
  f=callback(error)
  return try parallel:
    values=try await spawn map([42],f)
    values[0]
fn main()->i64 ! TaskError:
  return start() catch fn(error):
    return try recovered(error)
