fn divide(value:i64)->i64:
  return 10/value
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn run()->i64 ! TaskError:
  return try parallel:
    try await spawn apply(0,divide)
fn main()->i64:
  return run() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
