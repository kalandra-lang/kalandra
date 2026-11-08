fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn run(end:i64)->i64 ! TaskError:
  callbacks=[make(40)]
  return try parallel:
    values=try await spawn slice(callbacks,0,end)
    values[0](2)
fn main()->i64:
  return run(2) catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:42
          _:0
      _:0
