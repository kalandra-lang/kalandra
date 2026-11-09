fn double(value:i64)->i64:
  return value*2
fn twice(value:f64)->f64:
  return value*2
fn apply(value:i64,first:fn(i64)->i64,second:fn(f64)->f64)->i64:
  return first(value)+i64(second(f64(value)))-42
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn apply(21,double,twice)
