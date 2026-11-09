fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn forward(f:fn(i64)->i64,count:i64)->fn(i64)->i64:
  return if count==0:f
  else:forward(f,count-1)
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn forward(double,3)
  return f(21)
