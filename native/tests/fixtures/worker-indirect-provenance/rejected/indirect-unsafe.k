fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
type Outcome:
  error:fn(i64)->i64
fn unsafe(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn identity(f:fn(i64)->i64)->fn(i64)->i64:
  return f
fn consume(error:fn(i64)->i64)->i64 ! TaskError:
  factory=identity
  f=factory(unsafe(error))
  return try parallel:
    try await spawn apply(42,f)
fn main()->i64:
  return 42
