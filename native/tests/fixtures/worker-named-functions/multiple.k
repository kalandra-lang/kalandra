fn double(value:i64)->i64:
  return value*2
fn triple(value:i64)->i64:
  return value*3
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn combine(value:i64,first:fn(i64)->i64,second:fn(i64)->i64)->i64:
  return first(value)+second(value)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn combine(21,double,triple)-63
