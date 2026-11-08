type Outcome:
  error:fn(i64)->i64
error E:
  Retry:
    callback:fn(i64)->i64
fn unsafe(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn worker(error:fn(i64)->i64,flag:bool)->i64 ! E:
  fail E.Retry {callback:unsafe(error)}
fn submit(error:fn(i64)->i64)->i64 ! (E | TaskError):
  return try parallel:
    try await spawn worker(error,true)
fn main()->i64:
  return 42
