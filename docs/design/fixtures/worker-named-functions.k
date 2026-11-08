fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn main()->i64 ! TaskError:
  transform=double
  alias:fn(i64)->i64=transform
  return try parallel:
    local=alias
    work=spawn apply(21,local)
    first=try await work
    second=try await work
    first+second-42
