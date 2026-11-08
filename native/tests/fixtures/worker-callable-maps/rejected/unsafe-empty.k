type Outcome:
  error:fn(i64)->i64
fn unsafe(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn consume(error:fn(i64)->i64)->i64 ! TaskError:
  input:[i64]=[]
  f=unsafe(error)
  return try parallel:
    values=try await spawn map(input,f)
    length(values)
fn main()->i64:
  return 42
