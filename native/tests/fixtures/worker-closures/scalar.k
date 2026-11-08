fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  offset=40
  flag=true
  f=fn(value:i64):
    return if flag:value+offset
    else:0
  return try parallel:
    try await spawn apply(2,f)
