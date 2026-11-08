fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn make()->fn(i64)->i64:
  values=[double]
  return fn(value:i64):return values[0](value)
fn main()->i64 ! TaskError:
  f=try parallel:
    try await spawn make()
  return f(21)
