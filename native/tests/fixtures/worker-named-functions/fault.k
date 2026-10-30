fn divide(value:int)->int:
  10/value
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn run()->int ! TaskError:
  try parallel:
    try await spawn apply(0,divide)
fn main()->int:
  run() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Arithmetic:42
          _:0
      _:0
