error AppError:
  Bad
type Only=TaskError
type Mixed=AppError | TaskError
fn divide(x:int)->int:
  1/x
fn legacy()->int ! TaskError:
  try parallel:
    try await spawn divide(0)
fn failValue(error:Only)->Only ! TaskError:
  try parallel:
    number=try await spawn divide(0)
    error
fn work()->int ! Mixed:
  legacy() catch fn(error):
    result=parallel:
      try await spawn failValue(error)
    value=result catch fn(execution):execution
    fail value
fn main()->int:
  work() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          FaultKind.Native:99
          _:98
      _:97
