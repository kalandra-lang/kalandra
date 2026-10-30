fn divide(value:int)->int:
  1/value
fn run()->int ! TaskError:
  f=divide
  try parallel:
    values=try await spawn map([0],f)
    values[0]
fn main()->int:
  run() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
