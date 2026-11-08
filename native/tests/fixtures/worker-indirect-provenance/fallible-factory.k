fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
error E:
  Bad:
    value:i64
fn make(flag:bool)->(fn(i64)->i64) ! E:
  if flag:fail E.Bad {value:1}
  return double
fn main()->i64 ! (E | TaskError):
  factory=make
  f=try factory(false)
  return try parallel:
    try await spawn apply(21,f)
