fn double(value:i64)->i64:
  return value*2
fn wrong(value:f64)->f64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn main()->i64 ! TaskError:
  incompatible=wrong
  return try parallel:
    try await spawn apply(21,double)
