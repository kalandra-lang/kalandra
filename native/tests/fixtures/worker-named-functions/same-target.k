fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn main()->i64 ! TaskError:
  transform=double
  return try parallel:
    first=spawn apply(21,transform)
    second=spawn apply(21,transform)
    a=try await first
    b=try await second
    a+b-42
