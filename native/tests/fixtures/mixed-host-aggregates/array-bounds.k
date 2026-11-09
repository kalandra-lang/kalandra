type Pair:
  first:i64
  second:i64
fn read(index:i64)->i64:
  values=[Pair {first:17,second:25}]
  value=values[index]
  return value.first+value.second
fn run(index:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn read(index) on devices.gpu()
fn score(index:i64)->i64:
  return run(index) catch fn(error):
    return match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Bounds:100
          _:0
      _:0
fn main()->i64 ! TaskError:
  return score(-1)+score(0)+score(1)+score(9223372036854775807)
