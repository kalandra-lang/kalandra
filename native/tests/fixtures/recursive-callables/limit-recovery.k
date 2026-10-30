error E:
  Bad
fn descend(n:int)->int ! E:
  if n==0:fail E.Bad
  try descend(n-1)
fn worker(n:int)->int:
  callback=descend
  callback(n) catch fn(error):0
fn attempt()->int ! TaskError:
  try parallel:
    try await spawn worker(1000000)
fn main()->int:
  attempt() catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Native:42
          FaultKind.Allocation:42
          _:0
      _:0
