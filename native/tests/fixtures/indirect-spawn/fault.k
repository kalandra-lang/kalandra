fn double(value:int)->int:
  value*2
fn divide(value:int)->int:
  1/value
fn run()->int ! TaskError:
  f=divide
  try parallel:
    try await spawn f(0)
fn main()->int:
  run() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
