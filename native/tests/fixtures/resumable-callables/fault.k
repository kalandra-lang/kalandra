fn divide(value:i64)->i64:
  return 1/value
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn divide(value)
fn main()->i64:
  f=run
  return f(0) catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
