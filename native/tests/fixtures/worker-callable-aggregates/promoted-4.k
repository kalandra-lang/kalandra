fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
type Box:
  f:fn(i64)->i64
fn worker(box:Box)->i64:
  return box.f(21)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker(Box {f:double})
