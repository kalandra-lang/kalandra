fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  f=double
  return try parallel:
    values=try await spawn map([21],f)
    values[0]
