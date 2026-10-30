type Outcome:
  error:TaskError
fn divide(value:int)->int:
  1/value
fn start()->int ! TaskError:
  try parallel:
    try await spawn divide(0)
fn callback(error:TaskError)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    match outcome.error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:value
          _:0
      _:0
fn recovered(error:TaskError)->int ! TaskError:
  f=callback(error)
  try parallel:
    values=try await spawn map([42],f)
    values[0]
fn main()->int ! TaskError:
  start() catch fn(error):
    try recovered(error)
