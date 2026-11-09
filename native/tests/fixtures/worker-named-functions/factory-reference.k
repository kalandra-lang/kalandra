fn makeAdder(offset:i64)->fn(i64)->i64:
  return fn(value:i64):return value+offset
fn apply(value:i64,maker:fn(i64)->fn(i64)->i64)->i64:
  transform=maker(40)
  return transform(value)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn apply(2,makeAdder)
