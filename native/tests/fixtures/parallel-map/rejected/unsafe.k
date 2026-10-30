type Outcome:
  error:fn(int)->int
fn unsafe(error:fn(int)->int)->fn(int)->int:
  outcome=Outcome {error:error}
  fn(value:int):
    outcome.error(value)
fn run(error:fn(int)->int)->[int] ! TaskError:
  input:[int]=[]
  callback=unsafe(error)
  try parallelMap(input,callback)
fn main()->int:
  42
