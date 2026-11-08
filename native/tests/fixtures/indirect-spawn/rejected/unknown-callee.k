fn run(f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    try await spawn f(42)
fn main()->i64:
  return 42
