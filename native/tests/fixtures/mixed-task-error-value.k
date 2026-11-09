error AppError:
  Bad
type Only=TaskError
type Mixed=AppError | TaskError
fn divide(x:i64)->i64:
  return 1/x
fn legacy()->i64 ! TaskError:
  return try parallel:
    try await spawn divide(0)
fn failValue(error:Only)->Only ! TaskError:
  return try parallel:
    number=try await spawn divide(0)
    error
fn work()->i64 ! Mixed:
  return legacy() catch fn(error):
    result=parallel:
      try await spawn failValue(error)
    value=result catch fn(execution):return execution
    fail value
fn main()->i64:
  return work() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          FaultKind.Native:99
          _:98
      _:97
