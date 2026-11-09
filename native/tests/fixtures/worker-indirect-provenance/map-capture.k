fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  values=[40]
  functions=map([1],fn(offset):return fn(value:i64):return values[0]+offset+value)
  return try parallel:
    try await spawn apply(1,functions[0])
