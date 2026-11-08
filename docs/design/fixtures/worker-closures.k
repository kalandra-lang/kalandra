fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  return fn(value:i64):return values[0]+value
fn main()->i64 ! TaskError:
  first=make(40)
  second=make(20)
  return try parallel:
    a=spawn apply(2,first)
    b=spawn apply(22,second)
    x=try await a
    y=try await b
    x+y-42
