fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
type Box:
  f:fn(i64)->i64
fn run(flag:bool)->i64 ! TaskError:
  box=Box {f:double}
  f=if flag:double
  else:fn(value:i64):return box.f(value)
  return try parallel:
    try await spawn apply(21,f)
fn main()->i64 ! TaskError:
  return try run(true)
