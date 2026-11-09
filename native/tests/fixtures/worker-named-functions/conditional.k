fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn run(flag:bool)->i64 ! TaskError:
  transform=if flag:
    alias=double
    alias
  else:
    alias=triple
    alias
  return try parallel:
    try await spawn apply(14,transform)
fn main()->i64 ! TaskError:
  a=try run(false)
  b=try run(true)
  return a+b-28
