error E:
  Bad:
    value:i64
fn checked(value:i64)->i64 ! E:
  if value==0:fail E.Bad {value:42}
  return value*2
fn apply(value:i64,f:fn(i64)->i64 ! E)->i64 ! E:
  return try f(value)
fn forward(f:fn(i64)->i64 ! E)->i64 ! (E | TaskError):
  return try parallel:
    try await spawn apply(0,f)
fn main()->i64:
  return forward(checked) catch fn(error):
    return match error:
      E.Bad {value}:value
      _:0
