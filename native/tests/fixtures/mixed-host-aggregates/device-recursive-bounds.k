fn walk(n:int, index:int)->int:
  if n==0:
    values=[42]
    values[index]
  else:
    walk(n-1,index)+1
fn run(index:int)->int ! TaskError:
  try parallel:
    try await spawn walk(100,index) on devices.gpu()
fn score(index:int)->int:
  run(index) catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:1000
          _:0
      _:0
fn main()->int ! TaskError:
  score(-1)+score(0)+score(1)
