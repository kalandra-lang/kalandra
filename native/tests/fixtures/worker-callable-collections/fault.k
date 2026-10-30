fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn run(end:int)->int ! TaskError:
  callbacks=[make(40)]
  try parallel:
    values=try await spawn slice(callbacks,0,end)
    values[0](2)
fn main()->int:
  run(2) catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:42
          _:0
      _:0
