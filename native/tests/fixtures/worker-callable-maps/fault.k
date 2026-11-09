fn divide(value:i64)->i64:
  return 1/value
fn run()->i64 ! TaskError:
  f=divide
  return try parallel:
    values=try await spawn map([0],f)
    values[0]
fn main()->i64:
  return run() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
