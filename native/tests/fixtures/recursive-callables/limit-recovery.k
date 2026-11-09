error E:
  Bad
fn descend(n:i64)->i64 ! E:
  if n==0:fail E.Bad
  return try descend(n-1)
fn worker(n:i64)->i64:
  callback=descend
  return callback(n) catch fn(error):return 0
fn attempt()->i64 ! TaskError:
  return try parallel:
    try await spawn worker(1000000)
fn main()->i64:
  return attempt() catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Native:42
          FaultKind.Allocation:42
          _:0
      _:0
