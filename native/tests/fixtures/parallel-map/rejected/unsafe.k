type Outcome:
  error:fn(i64)->i64
fn unsafe(error:fn(i64)->i64)->fn(i64)->i64:
  outcome=Outcome {error:error}
  return fn(value:i64):
    return outcome.error(value)
fn run(error:fn(i64)->i64)->[i64] ! TaskError:
  input:[i64]=[]
  callback=unsafe(error)
  return try parallelMap(input,callback)
fn main()->i64:
  return 42
