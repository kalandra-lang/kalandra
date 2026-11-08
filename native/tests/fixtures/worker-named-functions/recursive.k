fn count(value:i64)->i64:
  return if value==0:
    22
  else:
    count(value-1)+1
fn apply(value:i64,transform:fn(i64)->i64)->i64:
  return transform(value)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn apply(20,count)
