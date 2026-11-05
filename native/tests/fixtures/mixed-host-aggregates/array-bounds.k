type Pair:
  first:int
  second:int
fn read(index:int)->int:
  values=[Pair {first:17,second:25}]
  value=values[index]
  value.first+value.second
fn run(index:int)->int ! TaskError:
  try parallel:
    try await spawn read(index) on devices.gpu()
fn score(index:int)->int:
  run(index) catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:100
          _:0
      _:0
fn main()->int ! TaskError:
  score(-1)+score(0)+score(1)+score(9223372036854775807)
