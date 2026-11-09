fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn worker(values:[fn(i64)->i64])->i64:
  return values[0](21)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn worker([double])
