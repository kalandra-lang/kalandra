type Outcome:
  error:fn(i64)->i64
fn unsafe(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn consume(error:fn(i64)->i64)->i64 ! TaskError:
  callbacks=[unsafe(error)]
  good=[fn(value:i64):return value]
  return try parallel:
    values=try await spawn concat(callbacks,good)
    length(values)
fn main()->i64:
  return 42
