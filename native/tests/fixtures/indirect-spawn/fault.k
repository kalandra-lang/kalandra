fn double(value:i64)->i64:
  return value*2
fn divide(value:i64)->i64:
  return 1/value
fn run()->i64 ! TaskError:
  f=divide
  return try parallel:
    try await spawn f(0)
fn main()->i64:
  return run() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
