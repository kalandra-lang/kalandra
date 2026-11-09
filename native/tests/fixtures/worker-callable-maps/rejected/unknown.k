fn consume(callback:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    values=try await spawn map([42],callback)
    values[0]
fn main()->i64:
  return 42
