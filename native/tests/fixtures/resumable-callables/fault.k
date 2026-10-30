fn divide(value:int)->int:
  1/value
fn run(value:int)->int ! TaskError:
  try parallel:
    try await spawn divide(value)
fn main()->int:
  f=run
  f(0) catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
