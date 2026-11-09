fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn iterate(factory:fn(fn(i64)->i64)->fn(i64)->i64,f:fn(i64)->i64,count:i64)->fn(i64)->i64:
  return if count==0:f
  else:iterate(factory,factory(f),count-1)
fn main()->i64 ! TaskError:
  f=iterate(identity,double,3)
  return try parallel:
    try await spawn apply(21,f)
