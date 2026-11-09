fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn forward(f:fn(i64)->i64)->fn(i64)->i64:
  alias=f
  return if true:alias
  else:double
fn main()->i64 ! TaskError:
  first=forward(double)
  second=forward(fn(value:i64):return value+21)
  return try parallel:
    a=spawn apply(21,first)
    b=spawn apply(21,second)
    ((try await a)+(try await b))/2
