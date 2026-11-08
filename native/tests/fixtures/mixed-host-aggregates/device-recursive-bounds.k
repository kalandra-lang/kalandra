fn walk(n:i64, index:i64)->i64:
  return if n==0:
    values=[42]
    values[index]
  else:
    walk(n-1,index)+1
fn run(index:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn walk(100,index) on devices.gpu()
fn score(index:i64)->i64:
  return run(index) catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:1000
          _:0
      _:0
fn main()->i64 ! TaskError:
  return score(-1)+score(0)+score(1)
