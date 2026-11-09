type Outcome:
  error:fn(i64)->i64
fn run(error:fn(i64)->i64)->i64 ! TaskError:
  outcome=Outcome {error:error}
  f=fn(value:i64):
    return outcome.error(value)
  return try parallel:
    try await spawn f(42)
fn main()->i64:
  return 42
