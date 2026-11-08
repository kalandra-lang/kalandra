type Outcome:
  error:fn(i64)->i64
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn wrap(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn forward(f:fn(i64)->i64)->i64 ! TaskError:
  return try parallel:
    try await spawn apply(42,f)
fn run(error:fn(i64)->i64)->i64 ! TaskError:
  return try forward(wrap(error))
fn main()->i64:
  return 42
