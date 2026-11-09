fn make(offset:i64)->fn(i64)->i64:
  values=[offset]
  base=fn(value:i64):return values[0]+value
  return fn(value:i64):return base(value)
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  first=make(10)
  second=make(20)
  return try parallel:
    left=spawn apply(5,first)
    right=spawn apply(7,second)
    (try await left)+(try await right)
