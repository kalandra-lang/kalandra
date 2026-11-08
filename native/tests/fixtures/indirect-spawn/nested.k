fn double(value:i64)->i64:
  return value*2
fn run(f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    a=spawn f(10)
    b=spawn f(11)
    (try await a)+(try await b)
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn run(double)
