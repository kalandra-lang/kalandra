fn double(value:i64)->i64:
  return value*2
fn worker(value:i64)->i64 ! TaskError:
  result=try parallelMap([value],double)
  return result[0]
fn main()->i64 ! TaskError:
  return try parallel:
    first=spawn worker(10)
    second=spawn worker(11)
    (try await first)+(try await second)
