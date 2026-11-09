fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
enum Mode:
  Twice
  Thrice
fn run(mode:Mode)->i64 ! TaskError:
  transform=match mode:
    Mode.Twice:double
    Mode.Thrice:triple
  return try parallel:
    try await spawn apply(14,transform)
fn main()->i64 ! TaskError:
  a=try run(Mode.Thrice)
  b=try run(Mode.Twice)
  return a+b-28
